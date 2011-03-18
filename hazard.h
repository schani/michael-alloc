#ifndef __MONO_UTILS_HAZARD_H__
#define __MONO_UTILS_HAZARD_H__

#include <glib.h>
#include <pthread.h>

typedef struct {
	gpointer hazard_pointers [2];
} MonoThreadHazardPointers;

typedef void (*MonoHazardousFreeFunc) (gpointer p);

void mono_thread_hazardous_free_or_queue (gpointer p, MonoHazardousFreeFunc free_func);
void mono_thread_hazardous_try_free_all (void);

MonoThreadHazardPointers* mono_hazard_pointer_get (void);

#define mono_hazard_pointer_set(hp,i,v)	\
	do { g_assert ((i) == 0 || (i) == 1); \
		(hp)->hazard_pointers [(i)] = (v); \
		mono_memory_write_barrier (); \
	} while (0)
#define mono_hazard_pointer_clear(hp,i)	\
	do { g_assert ((i) == 0 || (i) == 1); \
		(hp)->hazard_pointers [(i)] = NULL; \
	} while (0)

#endif
