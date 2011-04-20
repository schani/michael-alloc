#ifndef __MONO_UTILS_HAZARD_H__
#define __MONO_UTILS_HAZARD_H__

#include <glib.h>

#include "delayed-free.h"

typedef struct {
	gpointer hazard_pointers [2];
} MonoThreadHazardPointers;

void mono_thread_hazardous_free_or_queue (gpointer p, MonoHazardousFreeFunc free_func);
void mono_thread_hazardous_try_free_all (void);

MonoThreadHazardPointers* mono_hazard_pointer_get (void);

#define mono_hazard_pointer_set(hp,i,v)	\
	do { g_assert ((i) == 0 || (i) == 1); \
		g_assert (!(hp)->hazard_pointers [(i)]); \
		(hp)->hazard_pointers [(i)] = (v); \
		mono_memory_write_barrier (); \
	} while (0)
#define mono_hazard_pointer_clear(hp,i)	\
	do { g_assert ((i) == 0 || (i) == 1); \
		(hp)->hazard_pointers [(i)] = NULL; \
	} while (0)

gpointer mono_thread_hazardous_load (gpointer volatile *pp, MonoThreadHazardPointers *hp, int hazard_index);

void mono_thread_attach (void);

void mono_thread_hazardous_init (void);
void mono_thread_hazardous_print_stats (void);

#endif
