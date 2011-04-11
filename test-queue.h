#ifndef __MONO_UTILS_QUEUE_H__
#define __MONO_UTILS_QUEUE_H__

#define MONO_LOCK_FREE_QUEUE_SIZE	16

#define QUEUE_DEBUG	1

typedef struct _MonoLockFreeQueueNode MonoLockFreeQueueNode;

struct _MonoLockFreeQueueNode {
#ifdef QUEUE_DEBUG
	gint32 in_queue;
#endif
};

typedef struct {
	int index;
	MonoLockFreeQueueNode *entries [MONO_LOCK_FREE_QUEUE_SIZE];
} MonoLockFreeQueue;

void mono_lock_free_queue_init (MonoLockFreeQueue *q);

void mono_lock_free_queue_enqueue (MonoLockFreeQueue *q, MonoLockFreeQueueNode *node);

MonoLockFreeQueueNode* mono_lock_free_queue_dequeue (MonoLockFreeQueue *q);

#endif
