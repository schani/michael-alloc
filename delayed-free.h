#ifndef __MONO_UTILS_DELAYED_FREE_H__
#define __MONO_UTILS_DELAYED_FREE_H__

#include <glib.h>
#include <pthread.h>

#define CRITICAL_SECTION pthread_mutex_t
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock

typedef void (*MonoHazardousFreeFunc) (gpointer p);

typedef struct {
	gpointer p;
	MonoHazardousFreeFunc free_func;
} MonoDelayedFreeItem;

void mono_delayed_free_push (MonoDelayedFreeItem item);

gboolean mono_delayed_free_pop (MonoDelayedFreeItem *item);

void mono_delayed_free_init (void);

#endif
