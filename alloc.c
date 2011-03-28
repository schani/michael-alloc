#include <glib.h>
#include <unistd.h>
#include <stdlib.h>

#include "mono-mmap.h"
#include "mono-membar.h"
#include "hazard.h"
#include "atomic.h"
#include "queue.h"
#include "sgen-gc.h"

#define LAST_BYTE_DEBUG

#ifdef LAST_BYTE_DEBUG
#define LAST_BYTE(p,s)	(*((unsigned char*)p + (s) - 1))
#endif

enum {
	STATE_ACTIVE,
	STATE_FULL,
	STATE_PARTIAL,
	STATE_EMPTY
};

typedef union {
	guint64 value;
	struct {
		guint64 avail : 10;
		guint64 count : 10;
		guint64 state : 2;
		guint64 tag : 42;
	} data;
} Anchor;

typedef struct _Descriptor Descriptor;
typedef struct _ProcHeap ProcHeap;

struct _Descriptor {
	MonoLockFreeQueueNode node; /* Do we really need this? */
	ProcHeap *heap;
	Anchor anchor;
	unsigned int slot_size;
	unsigned int max_count;
	gpointer sb;
	Descriptor *next;
};

#define NUM_DESC_BATCH	64

#define SB_SIZE		16384
#define SB_HEADER_SIZE	16
#define SB_USABLE_SIZE	(SB_SIZE - SB_HEADER_SIZE)
#define MAX_SMALL_SIZE	(8192 - 8)

#define SB_HEADER_FOR_ADDR(a)	((gpointer)((mword)(a) & ~(mword)(SB_SIZE-1)))
#define DESCRIPTOR_FOR_ADDR(a)	(*(Descriptor**)SB_HEADER_FOR_ADDR (a))

typedef struct {
	MonoLockFreeQueue partial;
	unsigned int slot_size;
} SizeClass;

#define MAX_CREDITS	0x3f

#define ACTIVE_PTR(a)		((Descriptor*)((mword)(a) & ~(mword)MAX_CREDITS))
#define ACTIVE_CREDITS(a)	((mword)(a) & (mword)MAX_CREDITS)
#define ACTIVE_MAKE(p,c)	((Descriptor*)((mword)(p) | (mword)(c)))
#define ACTIVE_SET_CREDITS(a,c)	(ACTIVE_MAKE (ACTIVE_PTR ((a)), (c)))

struct _ProcHeap {
	Descriptor *active;	/* lowest 6 bits are used for credits */
	Descriptor *partial;
	SizeClass *sc;
};

static Descriptor * volatile desc_avail;

static gpointer
alloc_sb (Descriptor *desc)
{
	gpointer sb_header = mono_sgen_alloc_os_memory_aligned (SB_SIZE, SB_SIZE, TRUE);
	g_assert (sb_header == SB_HEADER_FOR_ADDR (sb_header));
	DESCRIPTOR_FOR_ADDR (sb_header) = desc;
	return (char*)sb_header + SB_HEADER_SIZE;
}

static void
free_sb (gpointer sb)
{
	gpointer sb_header = SB_HEADER_FOR_ADDR (sb);
	g_assert ((char*)sb_header + SB_HEADER_SIZE == sb);
	mono_sgen_free_os_memory (sb_header, SB_SIZE);
}

static Descriptor*
desc_alloc (void)
{
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();
	Descriptor *desc;

	for (;;) {
		gboolean success;

		desc = mono_thread_hazardous_load ((gpointer * volatile)&desc_avail, hp, 1);
		if (desc) {
			Descriptor *next = desc->next;
			success = (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, next, desc) == desc);
		} else {
			size_t desc_size = MAX (sizeof (Descriptor), MAX_CREDITS + 1);
			Descriptor *d;
			int i;

			desc = mono_sgen_alloc_os_memory (desc_size * NUM_DESC_BATCH, TRUE);

			/* Organize into linked list. */
			d = desc;
			for (i = 0; i < NUM_DESC_BATCH; ++i) {
				Descriptor *next = (i == (NUM_DESC_BATCH - 1)) ? NULL : (Descriptor*)((char*)desc + ((i + 1) * desc_size));
				d->next = next;
				d = next;
			}

			mono_memory_write_barrier ();

			success = (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, desc->next, NULL) == NULL);

			if (!success)
				mono_sgen_free_os_memory (desc, desc_size * NUM_DESC_BATCH);
		}

		mono_hazard_pointer_clear (hp, 1);

		if (success)
			break;
	}

	g_assert (ACTIVE_CREDITS (desc) == 0);

	return desc;
}

static void
desc_enqueue_avail (gpointer _desc)
{
	Descriptor *desc = _desc;
	Descriptor *old_head;

	do {
		old_head = desc_avail;
		desc->next = old_head;
		mono_memory_write_barrier ();
	} while (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, desc, old_head) != old_head);
}

static void
desc_retire (Descriptor *desc)
{
	mono_thread_hazardous_free_or_queue (desc, desc_enqueue_avail);
}

static Descriptor*
list_get_partial (SizeClass *sc)
{
	return (Descriptor*) mono_lock_free_queue_dequeue (&sc->partial);
}

static void
list_put_partial (Descriptor *desc)
{
	mono_lock_free_queue_enqueue (&desc->heap->sc->partial, &desc->node);
}

static void
list_remove_empty_desc (SizeClass *sc)
{
	int num_non_empty = 0;
	for (;;) {
		Descriptor *desc = (Descriptor*) mono_lock_free_queue_dequeue (&sc->partial);
		if (!desc)
			return;
		/*
		 * We don't need to read atomically because we're the
		 * only thread that references this descriptor.
		 */
		if (desc->anchor.data.state == STATE_EMPTY) {
			desc_retire (desc);
		} else {
			mono_lock_free_queue_enqueue (&sc->partial, &desc->node);
			if (++num_non_empty >= 2)
				return;
		}
	}
}

static Descriptor*
heap_get_partial (ProcHeap *heap)
{
	Descriptor *desc;

	do {
		desc = heap->partial;
		if (!desc)
			return list_get_partial (heap->sc);
	} while (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->partial, NULL, desc) != desc);
	return desc;
}

static void
heap_put_partial (Descriptor *desc)
{
	ProcHeap *heap = desc->heap;
	Descriptor *prev;

	do {
		prev = desc->heap->partial;
	} while (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->partial, desc, prev) != prev);

	if (prev)
		list_put_partial (prev);
}

static void
remove_empty_desc (ProcHeap *heap, Descriptor *desc)
{
	if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->partial, NULL, desc) == desc)
		desc_retire (desc);
	else
		list_remove_empty_desc (heap->sc);
}

static void
update_active (ProcHeap *heap, Descriptor *desc, unsigned int more_credits)
{
	Anchor old_anchor, new_anchor;
	Descriptor *new_active = ACTIVE_SET_CREDITS (desc, more_credits - 1);

	if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, new_active, NULL) == NULL)
		return;

	/*
	 * Someone installed another active sb.  Return credits to sb
	 * and make it partial.
	 */

	do {
		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);
		new_anchor.data.count += more_credits;
		new_anchor.data.state = STATE_PARTIAL;
	} while (atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) != old_anchor.value);

	heap_put_partial (desc);
}

static gpointer
alloc_from_active (ProcHeap *heap)
{
	Descriptor *old_active, *new_active, *desc;
	Anchor old_anchor, new_anchor;
	gpointer addr;
	unsigned int old_credits;
	unsigned int more_credits = 0;

	do {
		old_active = heap->active;
		if (!old_active)
			return NULL;

		old_credits = ACTIVE_CREDITS (old_active);
		if (old_credits == 0)
			new_active = NULL;
		else
			new_active = ACTIVE_MAKE (ACTIVE_PTR (old_active), old_credits - 1);
	} while (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, new_active, old_active) != old_active);

	desc = ACTIVE_PTR (old_active);

	do {
		unsigned int next;

		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);

		mono_memory_read_barrier ();

		addr = (char*)desc->sb + old_anchor.data.avail * desc->slot_size;

		next = *(unsigned int*)addr;
		/*
		 * Another thread might have allocated this slot
		 * already, so next could be invalid.  Even if it's
		 * within the range, it might still be invalid, of
		 * course, but we take the shortcut here if we know it
		 * is, for efficiency, not correctness.
		 */
		if (next >= SB_USABLE_SIZE / desc->slot_size)
			continue;

		new_anchor.data.avail = next;
		++new_anchor.data.tag;

		if (old_credits == 0) {
			g_assert (old_anchor.data.state == STATE_ACTIVE);
			if (old_anchor.data.count == 0) {
				new_anchor.data.state = STATE_FULL;
			} else {
				more_credits = MIN (old_anchor.data.count, MAX_CREDITS);
				new_anchor.data.count -= more_credits;
			}
		}
	} while (atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) != old_anchor.value);

	if (old_credits == 0 && old_anchor.data.count > 0)
		update_active (heap, desc, more_credits);

	return addr;
}

static gpointer
alloc_from_partial (ProcHeap *heap)
{
	Descriptor *desc;
	Anchor old_anchor, new_anchor;
	unsigned int more_credits = 0;
	gpointer addr;

 retry:
	desc = heap_get_partial (heap);
	if (!desc)
		return NULL;

	desc->heap = heap;
	do {
		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);

		if (old_anchor.data.state == STATE_EMPTY) {
			desc_retire (desc);
			goto retry;
		}

		g_assert (old_anchor.data.state == STATE_PARTIAL);
		g_assert (old_anchor.data.count > 0);

		more_credits = MIN (old_anchor.data.count - 1, MAX_CREDITS);

		new_anchor.data.count -= more_credits + 1;
		new_anchor.data.state = (more_credits > 0) ? STATE_ACTIVE : STATE_FULL;
	} while (atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) != old_anchor.value);

	do {
		unsigned int next;

		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);

		addr = (char*)desc->sb + old_anchor.data.avail * desc->slot_size;

		next = *(unsigned int*)addr;
		g_assert (next < SB_USABLE_SIZE / desc->slot_size);
		new_anchor.data.avail = next;
		++new_anchor.data.tag;
	} while (atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) != old_anchor.value);

	if (more_credits > 0)
		update_active (heap, desc, more_credits);

	return addr;
}

static gpointer
alloc_from_new_sb (ProcHeap *heap)
{
	Descriptor *new_active;
	unsigned int slot_size, count, credits, i;
	Descriptor *desc = desc_alloc ();

	desc->sb = alloc_sb (desc);

	slot_size = desc->slot_size = heap->sc->slot_size;
	count = SB_USABLE_SIZE / slot_size;

	/* Organize blocks into linked list. */
	for (i = 1; i < count - 1; ++i)
		*(unsigned int*)((char*)desc->sb + i * slot_size) = i + 1;

	desc->heap = heap;
	/*
	 * Setting avail to 1 because 0 is the block we're allocating
	 * right away.
	 */
	desc->anchor.data.avail = 1;
	desc->slot_size = heap->sc->slot_size;
	desc->max_count = count;

	credits = MIN (desc->max_count - 1, MAX_CREDITS) - 1;
	new_active = ACTIVE_MAKE (desc, credits);

	desc->anchor.data.count = (desc->max_count - 1) - (credits + 1);
	desc->anchor.data.state = STATE_ACTIVE;

	mono_memory_write_barrier ();

	if (InterlockedCompareExchangePointer ((gpointer * volatile)&heap->active, new_active, NULL) == NULL) {
		return desc->sb;
	} else {
		free_sb (desc->sb);
		desc_retire (desc);
		return NULL;
	}
}

#define TEST_SIZE	64

static SizeClass test_sc;
static ProcHeap test_heap;

static void
init_heap (void)
{
	mono_lock_free_queue_init (&test_sc.partial);
	test_sc.slot_size = TEST_SIZE;
	test_heap.sc = &test_sc;
}

static ProcHeap*
find_heap (size_t size)
{
	g_assert (size <= TEST_SIZE);

	g_assert (test_heap.sc == &test_sc);

	return &test_heap;
}

gpointer
mono_lock_free_alloc (size_t size)
{
	ProcHeap *heap;
	gpointer addr;

	if (size > MAX_SMALL_SIZE)
		return mono_sgen_alloc_os_memory (size, TRUE);

	heap = find_heap (size);
	g_assert (heap);

	for (;;) {

		addr = alloc_from_active (heap);
		if (addr)
			break;

		addr = alloc_from_partial (heap);
		if (addr)
			break;

		addr = alloc_from_new_sb (heap);
		if (addr)
			break;
	}

#ifdef LAST_BYTE_DEBUG
	g_assert (!LAST_BYTE (addr, heap->sc->slot_size));
	LAST_BYTE (addr, heap->sc->slot_size) = 1;
#endif

	return addr;
}

void
mono_lock_free_free (gpointer ptr, size_t size)
{
	Anchor old_anchor, new_anchor;
	Descriptor *desc;
	gpointer sb;
	ProcHeap *heap = NULL;

	if (size > MAX_SMALL_SIZE) {
		mono_sgen_free_os_memory (ptr, size);
		return;
	}

	desc = DESCRIPTOR_FOR_ADDR (ptr);
	sb = desc->sb;
	g_assert (SB_HEADER_FOR_ADDR (ptr) == SB_HEADER_FOR_ADDR (sb));

#ifdef LAST_BYTE_DEBUG
	g_assert (LAST_BYTE (ptr, desc->slot_size));
	LAST_BYTE (ptr, desc->slot_size) = 0;
#endif

	do {
		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);
		*(unsigned int*)ptr = old_anchor.data.avail;
		new_anchor.data.avail = ((char*)ptr - (char*)sb) / desc->slot_size;
		g_assert (new_anchor.data.avail < SB_USABLE_SIZE / desc->slot_size);

		if (old_anchor.data.state == STATE_FULL)
			new_anchor.data.state = STATE_PARTIAL;

		if (old_anchor.data.count == desc->max_count - 1) {
			heap = desc->heap;
			/* FIXME: instruction fence (?) */
			new_anchor.data.state = STATE_EMPTY;
		} else {
			++new_anchor.data.count;
		}

		mono_memory_write_barrier ();
	} while (atomic64_cmpxchg ((volatile gint64*)&desc->anchor.value, old_anchor.value, new_anchor.value) != old_anchor.value);

	if (new_anchor.data.state == STATE_EMPTY) {
		free_sb (sb);
		remove_empty_desc (heap, desc);
	} else if (old_anchor.data.state == STATE_FULL) {
		heap_put_partial (desc);
	}
}

#ifdef LAST_BYTE_DEBUG
static void
descriptor_check_consistency (Descriptor *desc, int more_credits)
{
	int count = desc->anchor.data.count + more_credits;
	int max_count = SB_USABLE_SIZE / desc->slot_size;
	gboolean linked [max_count];
	int i;
	unsigned int index;
	Descriptor *avail;

	for (avail = desc_avail; avail; avail = avail->next)
		g_assert (desc != avail);

	g_assert (desc->slot_size == desc->heap->sc->slot_size);

	switch (desc->anchor.data.state) {
	case STATE_ACTIVE:
		g_assert (count <= max_count);
		break;
	case STATE_FULL:
		g_assert (count == 0);
		break;
	case STATE_PARTIAL:
		g_assert (count < max_count);
		break;
	case STATE_EMPTY:
		g_assert (count == max_count);
		break;
	default:
		g_assert_not_reached ();
	}

	for (i = 0; i < max_count; ++i)
		linked [i] = FALSE;

	index = desc->anchor.data.avail;
	for (i = 0; i < count; ++i) {
		gpointer addr = (char*)desc->sb + index * desc->slot_size;
		g_assert (index >= 0 && index < max_count);
		g_assert (!linked [index]);
		g_assert (!LAST_BYTE (addr, desc->slot_size));
		linked [index] = TRUE;
		index = *(unsigned int*)addr;
	}

	for (i = 0; i < max_count; ++i) {
		gpointer addr = (char*)desc->sb + i * desc->slot_size;
		if (linked [i])
			continue;
		g_assert (LAST_BYTE (addr, desc->slot_size));
	}
}

static void
heap_check_consistency (ProcHeap *heap)
{
	Descriptor *active = ACTIVE_PTR (heap->active);
	Descriptor *desc;
	if (active) {
		int credits = ACTIVE_CREDITS (heap->active);
		g_assert (active->anchor.data.state == STATE_ACTIVE);
		descriptor_check_consistency (active, credits + 1);
	}
	if (heap->partial) {
		g_assert (heap->partial->anchor.data.state == STATE_PARTIAL);
		descriptor_check_consistency (heap->partial, 0);
	}
	while ((desc = (Descriptor*)mono_lock_free_queue_dequeue (&heap->sc->partial))) {
		g_assert (desc->anchor.data.state == STATE_PARTIAL || desc->anchor.data.state == STATE_EMPTY);
		descriptor_check_consistency (desc, 0);
	}

	g_print ("heap consistent\n");
	exit (0);
}
#endif

/* Test code */

#if 1

#define NUM_THREADS	2

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;
} ThreadData;

static ThreadData thread_datas [NUM_THREADS];

#define NUM_ENTRIES	256
#define NUM_ITERATIONS	100000000

static gpointer entries [NUM_ENTRIES];

static void
wait_for_threads_to_attach (void)
{
	int i;
 retry:
	for (i = 0; i < NUM_THREADS; ++i) {
		if (!thread_datas [i].have_attached) {
			usleep (5000);
			goto retry;
		}
	}
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int i, index;

	mono_thread_attach ();
	data->have_attached = TRUE;
	wait_for_threads_to_attach ();

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		gpointer p;
	retry:
		p = entries [index];
		if (p) {
			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], NULL, p) != p)
				goto retry;
			g_assert (*(int*)p == index);
			mono_lock_free_free (p, TEST_SIZE);
		} else {
			int j;

			p = mono_lock_free_alloc (TEST_SIZE);

			/*
			for (j = 0; j < NUM_ENTRIES; ++j)
				g_assert (entries [j] != p);
			*/

			*(int*)p = index;
			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], p, NULL) != NULL) {
				//g_print ("immediate free %p\n", p);
				mono_lock_free_free (p, TEST_SIZE);
				goto retry;
			}
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	return NULL;
}

int
main (void)
{
	int i;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	init_heap ();

	thread_datas [0].increment = 1;
	thread_datas [1].increment = 2;
	thread_datas [2].increment = 3;
	thread_datas [3].increment = 5;

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_create (&thread_datas [i].thread, NULL, thread_func, &thread_datas [i]);

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_join (thread_datas [i].thread, NULL);

	mono_thread_hazardous_try_free_all ();

	heap_check_consistency (&test_heap);

	return 0;
}

#endif
