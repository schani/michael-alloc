#ifndef __MONO_UTILS_QUEUE_H__
#define __MONO_UTILS_QUEUE_H__

#define QUEUE_DEBUG	1

typedef struct _MonoLockFreeQueueNode MonoLockFreeQueueNode;

struct _MonoLockFreeQueueNode {
	MonoLockFreeQueueNode *next;
#ifdef QUEUE_DEBUG
	gint32 in_queue;
#endif
};

typedef struct {
	MonoLockFreeQueueNode node;
	gint32 in_use;
} MonoLockFreeQueueDummy;

#define MONO_LOCK_FREE_QUEUE_NUM_DUMMIES	1

typedef struct {
	MonoLockFreeQueueNode *head;
	MonoLockFreeQueueNode *tail;
	MonoLockFreeQueueDummy dummies [MONO_LOCK_FREE_QUEUE_NUM_DUMMIES];
	gint32 has_dummy;
} MonoLockFreeQueue;

void mono_lock_free_queue_init (MonoLockFreeQueue *q);

void mono_lock_free_queue_enqueue (MonoLockFreeQueue *q, MonoLockFreeQueueNode *node);

MonoLockFreeQueueNode* mono_lock_free_queue_dequeue (MonoLockFreeQueue *q);

#endif
