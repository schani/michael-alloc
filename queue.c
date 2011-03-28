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

void
mono_lock_free_queue_init (MonoLockFreeQueue *q)
{
	q->dummy.next = NULL;
	q->head = q->tail = &q->dummy;
}

void
mono_lock_free_queue_enqueue (MonoLockFreeQueue *q, MonoLockFreeQueueNode *node)
{
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();
	MonoLockFreeQueueNode *tail;

#ifdef QUEUE_DEBUG
	if (node != &q->dummy) {
		g_assert (!node->in_queue);
		node->in_queue = TRUE;
		mono_memory_write_barrier ();
	}
#endif

	node->next = NULL;
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
			if (next == NULL) {
				if (InterlockedCompareExchangePointer ((gpointer volatile*)&tail->next, node, NULL) == NULL)
					break;
			} else {
				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, next, tail);
			}
		}

		mono_hazard_pointer_clear (hp, 0);
	}

	/* Try to advance tail */
	InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, node, tail);

	mono_hazard_pointer_clear (hp, 0);
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
			/* Is queue empty or tail behind? */
			if (head == tail) {
				if (next == NULL) {
					/* Queue is empty */
					mono_hazard_pointer_clear (hp, 0);
					/*
					 * We sometimes dequeue the
					 * dummy, so this does not
					 * necessarily hold.
					 */
					//g_assert (head == &q->dummy);
					return NULL;
				}
				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, next, tail);
			} else {
				g_assert (next);
				/* Try to dequeue head */
				if (InterlockedCompareExchangePointer ((gpointer volatile*)&q->head, next, head) == head)
					break;
			}
		}

		mono_hazard_pointer_clear (hp, 0);
	}

	head->next = NULL;

	/*
	 * The head is dequeued now, so we know it's this thread's
	 * responsibility to free it - no other thread can.
	 */
	mono_hazard_pointer_clear (hp, 0);

	if (head == &q->dummy) {
		mono_lock_free_queue_enqueue (q, &q->dummy);
		goto retry;
	}

#if QUEUE_DEBUG
	g_assert (head->in_queue);
	head->in_queue = FALSE;
	mono_memory_write_barrier ();
#endif

	/* The caller must hazardously free the node. */
	return head;
}

/* Test code */

#if 0

#define NUM_ENTRIES	32768
#define NUM_ITERATIONS	100000000

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
	if (e->mmap)
		return mmap (NULL, getpagesize (), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	else
		return g_malloc0 (sizeof (QueueEntry));
}

static void
free_entry_memory (QueueEntry *qe, gboolean mmap)
{
	if (mmap)
		munmap (qe, getpagesize ());
	else
		g_free (qe);
}

static void
free_entry (void *data)
{
	QueueEntry *e = data;
	g_assert (e->table_entry->queue_entry == e);
	e->table_entry->queue_entry = NULL;
	free_entry_memory (e, e->table_entry->mmap);
}

static void*
thread_func (void *data)
{
	int increment = (int)(long)data;
	int index;
	int i;

	mono_thread_attach ();

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
	pthread_t thread1, thread2, thread3, thread4;
	QueueEntry *qe;
	int i;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	mono_lock_free_queue_init (&queue);

	for (i = 0; i < NUM_ENTRIES; i += 97)
		entries [i].mmap = TRUE;

	pthread_create (&thread1, NULL, thread_func, (void*)1);
	pthread_create (&thread2, NULL, thread_func, (void*)2);
	pthread_create (&thread3, NULL, thread_func, (void*)3);
	pthread_create (&thread4, NULL, thread_func, (void*)5);

	pthread_join (thread1, NULL);
	pthread_join (thread2, NULL);
	pthread_join (thread3, NULL);
	pthread_join (thread4, NULL);

	mono_thread_hazardous_try_free_all ();

	while ((qe = (QueueEntry*)mono_lock_free_queue_dequeue (&queue)))
		free_entry (qe);

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (!entries [i].queue_entry);

	mono_thread_hazardous_print_stats ();

	return 0;
}

#endif
