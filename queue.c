/*
 * This is an implementation of a lock-free queue, as described in
 *
 * Simple, Fast, and Practical Non-Blocking and Blocking
 *   Concurrent Queue Algorithms
 * Maged M. Michael, Michael L. Scott
 * 1995
 *
 * A few slight modifications have been made:
 *
 * We use hazard pointers to rule out the ABA problem, instead of the
 * counter as in the paper.
 *
 * Memory management of the queue entries is done by the caller, not
 * by the queue implementation.  This implies that the dequeue
 * function must return the queue entry, not just the data.
 * Therefore, the dummy entry must never be returned.  We do this by
 * re-enqueuing the dummy entry after we dequeue it and then retrying
 * the dequeue.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include "mono-membar.h"
#include "hazard.h"
#include "atomic.h"

#include "queue.h"

#define INVALID_NEXT	((void*)-1)
#define END_MARKER	((void*)-2)
#define FREE_NEXT	((void*)-3)

void
mono_lock_free_queue_init (MonoLockFreeQueue *q)
{
	int i;
	for (i = 0; i < MONO_LOCK_FREE_QUEUE_NUM_DUMMIES; ++i) {
		q->dummies [i].node.next = (i == 0) ? END_MARKER : FREE_NEXT;
		q->dummies [i].in_use = i == 0 ? 1 : 0;
#ifdef QUEUE_DEBUG
		q->dummies [i].node.in_queue = i == 0 ? TRUE : FALSE;
#endif
	}

	q->head = q->tail = &q->dummies [0].node;
	q->has_dummy = 1;
}

void
mono_lock_free_queue_node_init (MonoLockFreeQueueNode *node, gboolean to_be_freed)
{
	node->next = to_be_freed ? INVALID_NEXT : FREE_NEXT;
#ifdef QUEUE_DEBUG
	node->in_queue = FALSE;
#endif
}

void
mono_lock_free_queue_node_free (MonoLockFreeQueueNode *node)
{
	g_assert (node->next == INVALID_NEXT);
#ifdef QUEUE_DEBUG
	g_assert (!node->in_queue);
#endif
	node->next = FREE_NEXT;
}

void
mono_lock_free_queue_enqueue (MonoLockFreeQueue *q, MonoLockFreeQueueNode *node)
{
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();
	MonoLockFreeQueueNode *tail;

#ifdef QUEUE_DEBUG
	g_assert (!node->in_queue);
	node->in_queue = TRUE;
	mono_memory_write_barrier ();
#endif

	g_assert (node->next == FREE_NEXT);
	node->next = END_MARKER;
	for (;;) {
		MonoLockFreeQueueNode *next;

		tail = mono_thread_hazardous_load ((gpointer volatile*)&q->tail, hp, 0);
		mono_memory_read_barrier ();
		/*
		 * We never dereference next so we don't need a
		 * hazardous load.
		 */
		next = tail->next;
		mono_memory_read_barrier ();

		/* Are tail and next consistent? */
		if (tail == q->tail) {
			g_assert (next != INVALID_NEXT && next != FREE_NEXT);
			g_assert (next != tail);

			if (next == END_MARKER) {
				/*
				 * Here we require that nodes that
				 * have been dequeued don't have
				 * next==END_MARKER.  If they did, we
				 * might append to a node that isn't
				 * in the queue anymore here.
				 */
				if (InterlockedCompareExchangePointer ((gpointer volatile*)&tail->next, node, END_MARKER) == END_MARKER)
					break;
			} else {
				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, next, tail);
			}
		}

		mono_memory_write_barrier ();
		mono_hazard_pointer_clear (hp, 0);
	}

	/* Try to advance tail */
	InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, node, tail);

	mono_memory_write_barrier ();
	mono_hazard_pointer_clear (hp, 0);
}

static void
free_dummy (gpointer _dummy)
{
	MonoLockFreeQueueDummy *dummy = _dummy;
	mono_lock_free_queue_node_free (&dummy->node);
	g_assert (dummy->in_use);
	mono_memory_write_barrier ();
	dummy->in_use = 0;
}

static MonoLockFreeQueueDummy*
get_dummy (MonoLockFreeQueue *q)
{
	int i;
	for (i = 0; i < MONO_LOCK_FREE_QUEUE_NUM_DUMMIES; ++i) {
		MonoLockFreeQueueDummy *dummy = &q->dummies [i];

		if (dummy->in_use)
			continue;

		if (InterlockedCompareExchange (&dummy->in_use, 1, 0) == 0)
			return dummy;
	}
	return NULL;
}

static gboolean
is_dummy (MonoLockFreeQueue *q, MonoLockFreeQueueNode *n)
{
	return n >= &q->dummies [0].node && n < &q->dummies [MONO_LOCK_FREE_QUEUE_NUM_DUMMIES].node;
}

static gboolean
try_reenqueue_dummy (MonoLockFreeQueue *q)
{
	MonoLockFreeQueueDummy *dummy;

	if (q->has_dummy)
		return FALSE;

	dummy = get_dummy (q);
	if (!dummy)
		return FALSE;

	if (InterlockedCompareExchange (&q->has_dummy, 1, 0) != 0) {
		dummy->in_use = 0;
		return FALSE;
	}

	mono_lock_free_queue_enqueue (q, &dummy->node);

	return TRUE;
}

MonoLockFreeQueueNode*
mono_lock_free_queue_dequeue (MonoLockFreeQueue *q)
{
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();
	MonoLockFreeQueueNode *head;

 retry:
	for (;;) {
		MonoLockFreeQueueNode *tail, *next;

		head = mono_thread_hazardous_load ((gpointer volatile*)&q->head, hp, 0);
		tail = q->tail;
		mono_memory_read_barrier ();
		next = head->next;
		mono_memory_read_barrier ();

		/* Are head, tail and next consistent? */
		if (head == q->head) {
			g_assert (next != INVALID_NEXT && next != FREE_NEXT);
			g_assert (next != head);

			/* Is queue empty or tail behind? */
			if (head == tail) {
				if (next == END_MARKER) {
					/* Queue is empty */
					mono_hazard_pointer_clear (hp, 0);

					/*
					 * We only continue if we
					 * reenqueue the dummy
					 * ourselves, so as not to
					 * wait for threads that might
					 * not actually run.
					 */
					if (!is_dummy (q, head) && try_reenqueue_dummy (q))
						continue;

					return NULL;
				}

				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, next, tail);
			} else {
				g_assert (next != END_MARKER);
				/* Try to dequeue head */
				if (InterlockedCompareExchangePointer ((gpointer volatile*)&q->head, next, head) == head)
					break;
			}
		}

		mono_memory_write_barrier ();
		mono_hazard_pointer_clear (hp, 0);
	}

	/*
	 * The head is dequeued now, so we know it's this thread's
	 * responsibility to free it - no other thread can.
	 */
	mono_memory_write_barrier ();
	mono_hazard_pointer_clear (hp, 0);

	g_assert (head->next);
	/*
	 * Setting next here isn't necessary for correctness, but we
	 * do it to make sure that we catch dereferencing next in a
	 * node that's not in the queue anymore.
	 */
	head->next = INVALID_NEXT;
#if QUEUE_DEBUG
	g_assert (head->in_queue);
	head->in_queue = FALSE;
	mono_memory_write_barrier ();
#endif

	if (is_dummy (q, head)) {
		g_assert (q->has_dummy);
		q->has_dummy = 0;
		mono_memory_write_barrier ();
		mono_thread_hazardous_free_or_queue (head, free_dummy);
		if (try_reenqueue_dummy (q))
			goto retry;
		return NULL;
	}

	/* The caller must hazardously free the node. */
	return head;
}

/* Test code */

#ifdef TEST_QUEUE

#define NUM_ENTRIES	16
#define NUM_ITERATIONS	10000000

typedef struct _TableEntry TableEntry;

typedef struct {
	MonoLockFreeQueueNode node;
	TableEntry *table_entry;
} QueueEntry;

struct _TableEntry {
	gboolean mmap;
	QueueEntry *queue_entry;
};

static MonoLockFreeQueue queue;
static TableEntry entries [NUM_ENTRIES];

static QueueEntry*
alloc_entry (TableEntry *e)
{
	QueueEntry *qe;

	if (e->mmap)
		qe = mmap (NULL, getpagesize (), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	else
		qe = g_malloc0 (sizeof (QueueEntry));

	mono_lock_free_queue_node_init (&qe->node);

	return qe;
}

static void
free_entry_memory (QueueEntry *qe, gboolean mmap)
{
	if (mmap)
		munmap (qe, getpagesize ());
	else
		g_free (qe);
}

#define NUM_THREADS	4

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;
} ThreadData;

static ThreadData thread_datas [NUM_THREADS];

static void
free_entry (void *data)
{
	QueueEntry *e = data;
	g_assert (e->table_entry->queue_entry == e);
	e->table_entry->queue_entry = NULL;
	free_entry_memory (e, e->table_entry->mmap);
}

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
	int index;
	int i;

	mono_thread_attach ();
	data->have_attached = TRUE;
	wait_for_threads_to_attach ();

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		TableEntry *e = &entries [index];

		if (e->queue_entry) {
			QueueEntry *qe = (QueueEntry*)mono_lock_free_queue_dequeue (&queue);
			if (qe) {
				/*
				 * Calling free_entry() directly here
				 * effectively disables hazardous
				 * pointers.  The test will then crash
				 * sooner or later.
				 */
				mono_thread_hazardous_free_or_queue (qe, free_entry);
				//free_entry (qe);
			}
		} else {
			QueueEntry *qe = alloc_entry (e);
			qe->table_entry = e;
			if (InterlockedCompareExchangePointer ((gpointer volatile*)&e->queue_entry, qe, NULL) == NULL) {
				mono_lock_free_queue_enqueue (&queue, &qe->node);
			} else {
				qe->table_entry = NULL;
				free_entry_memory (qe, e->mmap);
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
	QueueEntry *qe;
	int i;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	mono_lock_free_queue_init (&queue);

	for (i = 0; i < NUM_ENTRIES; i += 97)
		entries [i].mmap = TRUE;

	thread_datas [0].increment = 1;
	if (NUM_THREADS >= 2)
		thread_datas [1].increment = 2;
	if (NUM_THREADS >= 3)
		thread_datas [2].increment = 3;
	if (NUM_THREADS >= 4)
		thread_datas [3].increment = 5;

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_create (&thread_datas [i].thread, NULL, thread_func, &thread_datas [i]);

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_join (thread_datas [i].thread, NULL);

	mono_thread_hazardous_try_free_all ();

	while ((qe = (QueueEntry*)mono_lock_free_queue_dequeue (&queue)))
		free_entry (qe);

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (!entries [i].queue_entry);

	mono_thread_hazardous_print_stats ();

	return 0;
}

#endif
