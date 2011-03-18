#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

#include "mono-membar.h"
#include "hazard.h"

#define mono_pagesize getpagesize

static struct {
	long long hazardous_pointer_count;
} mono_stats;

#define CRITICAL_SECTION pthread_mutex_t
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock

typedef struct {
	int small_id;
} MonoInternalThread;

typedef struct {
	gpointer p;
	MonoHazardousFreeFunc free_func;
} DelayedFreeItem;

static CRITICAL_SECTION small_id_mutex;
static int small_id_table_size = 0;
static int small_id_next = 0;
static int highest_small_id = -1;
static MonoInternalThread **small_id_table = NULL;

/* The hazard table */
#if MONO_SMALL_CONFIG
#define HAZARD_TABLE_MAX_SIZE	256
#else
#define HAZARD_TABLE_MAX_SIZE	16384 /* There cannot be more threads than this number. */
#endif
static volatile int hazard_table_size = 0;
static MonoThreadHazardPointers * volatile hazard_table = NULL;

/* The table where we keep pointers to blocks to be freed but that
   have to wait because they're guarded by a hazard pointer. */
static CRITICAL_SECTION delayed_free_table_mutex;
static GArray *delayed_free_table = NULL;

static void*
mono_gc_alloc_fixed (size_t size, void *dummy)
{
	g_assert (dummy == NULL);
	return malloc (size);
}

static void
mono_gc_free_fixed (void *ptr)
{
	free (ptr);
}

#define mono_mprotect	mprotect
#define MONO_MMAP_NONE	PROT_NONE
#define MONO_MMAP_READ	PROT_READ
#define MONO_MMAP_WRITE	PROT_WRITE

static void*
mono_valloc (void *addr, size_t len, int prot)
{
	addr = mmap (addr, len, prot, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == (void*)-1) {
		fprintf (stderr, "mmap error: %m\n");
		return NULL;
	}
	return addr;
}

/*
 * Allocate a small thread id.
 *
 * FIXME: The biggest part of this function is very similar to
 * domain_id_alloc() in domain.c and should be merged.
 */
static int
small_id_alloc (MonoInternalThread *thread)
{
	int id = -1, i;

	EnterCriticalSection (&small_id_mutex);

	if (!small_id_table) {
		small_id_table_size = 2;
		/* 
		 * Enabling this causes problems, because SGEN doesn't track/update the TLS slot holding
		 * the current thread.
		 */
		//small_id_table = mono_gc_alloc_fixed (small_id_table_size * sizeof (MonoInternalThread*), mono_gc_make_root_descr_all_refs (small_id_table_size));
		small_id_table = mono_gc_alloc_fixed (small_id_table_size * sizeof (MonoInternalThread*), NULL);
	}
	for (i = small_id_next; i < small_id_table_size; ++i) {
		if (!small_id_table [i]) {
			id = i;
			break;
		}
	}
	if (id == -1) {
		for (i = 0; i < small_id_next; ++i) {
			if (!small_id_table [i]) {
				id = i;
				break;
			}
		}
	}
	if (id == -1) {
		MonoInternalThread **new_table;
		int new_size = small_id_table_size * 2;
		if (new_size >= (1 << 16))
			g_assert_not_reached ();
		id = small_id_table_size;
		//new_table = mono_gc_alloc_fixed (new_size * sizeof (MonoInternalThread*), mono_gc_make_root_descr_all_refs (new_size));
		new_table = mono_gc_alloc_fixed (new_size * sizeof (MonoInternalThread*), NULL);
		memcpy (new_table, small_id_table, small_id_table_size * sizeof (void*));
		mono_gc_free_fixed (small_id_table);
		small_id_table = new_table;
		small_id_table_size = new_size;
	}
	thread->small_id = id;
	g_assert (small_id_table [id] == NULL);
	small_id_table [id] = thread;
	small_id_next++;
	if (small_id_next > small_id_table_size)
		small_id_next = 0;

	g_assert (id < HAZARD_TABLE_MAX_SIZE);
	if (id >= hazard_table_size) {
#if MONO_SMALL_CONFIG
		hazard_table = g_malloc0 (sizeof (MonoThreadHazardPointers) * HAZARD_TABLE_MAX_SIZE);
		hazard_table_size = HAZARD_TABLE_MAX_SIZE;
#else
		gpointer page_addr;
		int pagesize = mono_pagesize ();
		int num_pages = (hazard_table_size * sizeof (MonoThreadHazardPointers) + pagesize - 1) / pagesize;

		if (hazard_table == NULL) {
			hazard_table = mono_valloc (NULL,
				sizeof (MonoThreadHazardPointers) * HAZARD_TABLE_MAX_SIZE,
				MONO_MMAP_NONE);
		}

		g_assert (hazard_table != NULL);
		page_addr = (guint8*)hazard_table + num_pages * pagesize;

		mono_mprotect (page_addr, pagesize, MONO_MMAP_READ | MONO_MMAP_WRITE);

		++num_pages;
		hazard_table_size = num_pages * pagesize / sizeof (MonoThreadHazardPointers);

#endif
		g_assert (id < hazard_table_size);
		hazard_table [id].hazard_pointers [0] = NULL;
		hazard_table [id].hazard_pointers [1] = NULL;
	}

	if (id > highest_small_id) {
		highest_small_id = id;
		mono_memory_write_barrier ();
	}

	LeaveCriticalSection (&small_id_mutex);

	return id;
}

static void
small_id_free (int id)
{
	g_assert (id >= 0 && id < small_id_table_size);
	g_assert (small_id_table [id] != NULL);

	small_id_table [id] = NULL;
}

static gboolean
is_pointer_hazardous (gpointer p)
{
	int i;
	int highest = highest_small_id;

	g_assert (highest < hazard_table_size);

	for (i = 0; i <= highest; ++i) {
		if (hazard_table [i].hazard_pointers [0] == p
				|| hazard_table [i].hazard_pointers [1] == p)
			return TRUE;
	}

	return FALSE;
}

static __thread MonoInternalThread this_internal_thread;

static MonoInternalThread*
mono_thread_internal_current (void)
{
	return &this_internal_thread;
}

MonoThreadHazardPointers*
mono_hazard_pointer_get (void)
{
	MonoInternalThread *current_thread = mono_thread_internal_current ();

	if (!(current_thread && current_thread->small_id >= 0)) {
		static MonoThreadHazardPointers emerg_hazard_table;
		g_warning ("Thread %p may have been prematurely finalized", current_thread);
		return &emerg_hazard_table;
	}

	return &hazard_table [current_thread->small_id];
}

static void
try_free_delayed_free_item (int index)
{
	if (delayed_free_table->len > index) {
		DelayedFreeItem item = { NULL, NULL };

		EnterCriticalSection (&delayed_free_table_mutex);
		/* We have to check the length again because another
		   thread might have freed an item before we acquired
		   the lock. */
		if (delayed_free_table->len > index) {
			item = g_array_index (delayed_free_table, DelayedFreeItem, index);

			if (!is_pointer_hazardous (item.p))
				g_array_remove_index_fast (delayed_free_table, index);
			else
				item.p = NULL;
		}
		LeaveCriticalSection (&delayed_free_table_mutex);

		if (item.p != NULL)
			item.free_func (item.p);
	}
}

void
mono_thread_hazardous_free_or_queue (gpointer p, MonoHazardousFreeFunc free_func)
{
	int i;

	/* First try to free a few entries in the delayed free
	   table. */
	for (i = 2; i >= 0; --i)
		try_free_delayed_free_item (i);

	/* Now see if the pointer we're freeing is hazardous.  If it
	   isn't, free it.  Otherwise put it in the delay list. */
	if (is_pointer_hazardous (p)) {
		DelayedFreeItem item = { p, free_func };

		++mono_stats.hazardous_pointer_count;

		EnterCriticalSection (&delayed_free_table_mutex);
		g_array_append_val (delayed_free_table, item);
		LeaveCriticalSection (&delayed_free_table_mutex);
	} else
		free_func (p);
}

void
mono_thread_hazardous_try_free_all (void)
{
	int len;
	int i;

	if (!delayed_free_table)
		return;

	len = delayed_free_table->len;

	for (i = len - 1; i >= 0; --i)
		try_free_delayed_free_item (i);
}

void
mono_thread_attach (void)
{
	small_id_alloc (mono_thread_internal_current ());
}

void
mono_thread_hazardous_init (void)
{
	pthread_mutex_init (&small_id_mutex, NULL);
	pthread_mutex_init (&delayed_free_table_mutex, NULL);
}

int
main (void)
{
	mono_thread_hazardous_init ();

	mono_thread_attach ();

	return 0;
}
