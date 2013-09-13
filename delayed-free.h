#ifndef __MONO_UTILS_DELAYED_FREE_H__
#define __MONO_UTILS_DELAYED_FREE_H__

#include <pthread.h>

#define CRITICAL_SECTION pthread_mutex_t
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock

#endif
