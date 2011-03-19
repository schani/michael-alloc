#include "hazard.h"
#include "atomic.h"

typedef struct _MonoLockFreeQueueNode MonoLockFreeQueueNode;

struct _MonoLockFreeQueueNode {
	MonoLockFreeQueueNode *next;
};

typedef struct {
	MonoLockFreeQueueNode *head;
	MonoLockFreeQueueNode *tail;
	MonoLockFreeQueueNode dummy;
} MonoLockFreeQueue;

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

	node->next = NULL;
	for (;;) {
		MonoLockFreeQueueNode *next;

		tail = mono_thread_hazardous_load ((gpointer volatile*)&q->tail, hp, 0);
		/*
		 * We never dereference next so we don't need a
		 * hazardous load.
		 */
		next = tail->next;

		/* Are tail and next consistent? */
		if (tail == q->tail) {
			if (next == NULL) {
				if (InterlockedCompareExchangePointer ((gpointer volatile*)&tail->next, node, NULL) == NULL)
					break;
			} else {
				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, next, tail);
				mono_hazard_pointer_clear (hp, 0);
			}
		}
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
		next = head->next;

		/* Are head, tail and next consistent? */
		if (head == q->head) {
			/* Is queue empty or tail behind? */
			if (head == tail) {
				if (next == NULL) {
					/* Queue is empty */
					mono_hazard_pointer_clear (hp, 0);
					g_assert (head == &q->dummy);
					return NULL;
				}
				/* Try to advance tail */
				InterlockedCompareExchangePointer ((gpointer volatile*)&q->tail, tail, next);
			} else {
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

	/* The caller must hazardously free the node. */
	return head;
}

#define NUM_ENTRIES	32768
#define NUM_ITERATIONS	100000000

typedef struct {
	MonoLockFreeQueueNode node;
	gint32 in_queue;
} Entry;

static MonoLockFreeQueue queue;
static Entry entries [NUM_ENTRIES];

static void
free_entry (void *data)
{
	Entry *e = data;
	if (InterlockedCompareExchange (&e->in_queue, 0, 1) != 1)
		g_assert_not_reached ();
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
		Entry *e = &entries [index];

		if (e->in_queue) {
			e = (Entry*)mono_lock_free_queue_dequeue (&queue);
			if (e)
				mono_thread_hazardous_free_or_queue (e, free_entry);
		} else {
			if (InterlockedCompareExchange (&e->in_queue, 1, 0) == 0)
				mono_lock_free_queue_enqueue (&queue, &e->node);
		}

		index += increment;
		if (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	return NULL;
}

int
main (void)
{
	pthread_t thread1, thread2, thread3, thread4;
	Entry *e;
	int i;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	mono_lock_free_queue_init (&queue);

	pthread_create (&thread1, NULL, thread_func, (void*)1);
	pthread_create (&thread2, NULL, thread_func, (void*)2);
	pthread_create (&thread3, NULL, thread_func, (void*)3);
	pthread_create (&thread4, NULL, thread_func, (void*)5);

	pthread_join (thread1, NULL);
	pthread_join (thread2, NULL);
	pthread_join (thread3, NULL);
	pthread_join (thread4, NULL);

	mono_thread_hazardous_try_free_all ();

	while ((e = (Entry*)mono_lock_free_queue_dequeue (&queue))) {
		g_assert (e->in_queue);
		e->in_queue = 0;
	}

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (!entries [i].in_queue);

	mono_thread_hazardous_print_stats ();

	return 0;
}
