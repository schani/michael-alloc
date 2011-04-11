#include "atomic.h"

#include "test-queue.h"

void
mono_lock_free_queue_init (MonoLockFreeQueue *q)
{
	int i;
	for (i = 0; i < MONO_LOCK_FREE_QUEUE_SIZE; ++i)
		q->entries [i] = NULL;
	q->index = 0;
}

void
mono_lock_free_queue_enqueue (MonoLockFreeQueue *q, MonoLockFreeQueueNode *node)
{
	int i, j;

	g_assert (!node->in_queue);
	node->in_queue = TRUE;

	j = 0;
	for (i = q->index; ; i = (i + 1) % MONO_LOCK_FREE_QUEUE_SIZE) {
		if (!q->entries [i]) {
			if (InterlockedCompareExchangePointer ((gpointer volatile*)&q->entries [i], node, NULL) == NULL) {
				if (j > MONO_LOCK_FREE_QUEUE_SIZE)
					g_print ("queue iterations: %d\n", j);
				return;
			}
		}
		++j;
	}
}

MonoLockFreeQueueNode*
mono_lock_free_queue_dequeue (MonoLockFreeQueue *q)
{
	int index = q->index;
	int i;

	for (i = (index + 1) % MONO_LOCK_FREE_QUEUE_SIZE; i != index; i = (i + 1) % MONO_LOCK_FREE_QUEUE_SIZE) {
		MonoLockFreeQueueNode *node = q->entries [i];
		if (node) {
			if (InterlockedCompareExchangePointer ((gpointer volatile*)&q->entries [i], NULL, node) == node) {
				g_assert (node->in_queue);
				node->in_queue = FALSE;
				return node;
			}
		}
	}

	return NULL;
}
