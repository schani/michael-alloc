#include <glib.h>

#include "mono-mmap.h"
#include "mono-membar.h"
#include "hazard.h"
#include "atomic.h"
#include "queue.h"
#include "sgen-gc.h"

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

		desc = mono_thread_hazardous_load ((gpointer * volatile)&desc_avail, hp, 0);
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
				Descriptor *next = (i == (NUM_DESC_BATCH - 1)) ? desc : (Descriptor*)((char*)desc + ((i + 1) * desc_size));
				*(Descriptor**)d = next;
				d = next;
			}

			mono_memory_write_barrier ();

			success = (InterlockedCompareExchangePointer ((gpointer * volatile)&desc_avail, desc->next, NULL) == NULL);

			if (!success)
				mono_sgen_free_os_memory (desc, desc_size * NUM_DESC_BATCH);
		}

		mono_hazard_pointer_clear (hp, 0);

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

		addr = (char*)desc->sb + old_anchor.data.avail * desc->slot_size;

		next = *(unsigned int*)addr;
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
	count = SB_SIZE / slot_size;

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

	if (size > MAX_SMALL_SIZE)
		return mono_sgen_alloc_os_memory (size, TRUE);

	heap = find_heap (size);
	g_assert (heap);

	for (;;) {
		gpointer addr;

		addr = alloc_from_active (heap);
		if (addr)
			return addr;

		addr = alloc_from_partial (heap);
		if (addr)
			return addr;

		addr = alloc_from_new_sb (heap);
		if (addr)
			return addr;
	}
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

	do {
		new_anchor = old_anchor = (Anchor)(guint64)atomic64_read ((gint64*)&desc->anchor);
		*(unsigned int*)ptr = old_anchor.data.avail;
		new_anchor.data.avail = ((char*)ptr - (char*)sb) / desc->slot_size;

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

/* Test code */

#if 1

#define NUM_ENTRIES	32768
#define NUM_ITERATIONS	100000000

static gpointer entries [NUM_ENTRIES];

static void*
thread_func (void *data)
{
	int increment = (int)(long)data;
	int i;
	gpointer p;

	for (i = 0; i < 1000000; ++i) {
		p = mono_lock_free_alloc (TEST_SIZE);
		g_assert (p);
		mono_lock_free_free (p, TEST_SIZE);
	}

	return NULL;
}

int
main (void)
{
	pthread_t thread1, thread2, thread3, thread4;
	int i, index;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	init_heap ();

	/*
	pthread_create (&thread1, NULL, thread_func, (void*)1);
	pthread_create (&thread2, NULL, thread_func, (void*)2);
	pthread_create (&thread3, NULL, thread_func, (void*)3);
	pthread_create (&thread4, NULL, thread_func, (void*)5);

	pthread_join (thread1, NULL);
	pthread_join (thread2, NULL);
	pthread_join (thread3, NULL);
	pthread_join (thread4, NULL);
	*/

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		gpointer p = entries [index];
		if (p) {
			g_assert (*(int*)p == index);
			mono_lock_free_free (p, TEST_SIZE);
			entries [index] = NULL;
		} else {
			p = mono_lock_free_alloc (TEST_SIZE);
			*(int*)p = index;
			entries [index] = p;
		}

		++index;
		if (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	mono_thread_hazardous_try_free_all ();

	return 0;
}

#endif
