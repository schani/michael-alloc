#ifndef __MONO_UTILS_HAZARD_H__
#define __MONO_UTILS_HAZARD_H__

#include <glib.h>

#include "delayed-free.h"
#include "mono-membar.h"

#define HAZARD_POINTER_COUNT 3

typedef struct {
	gpointer hazard_pointers [HAZARD_POINTER_COUNT];
} MonoThreadHazardPointers;

void mono_thread_hazardous_free_or_queue (gpointer p, MonoHazardousFreeFunc free_func);
void mono_thread_hazardous_try_free_all (void);
MonoThreadHazardPointers* mono_hazard_pointer_get (void);
gpointer get_hazardous_pointer (gpointer volatile *pp, MonoThreadHazardPointers *hp, int hazard_index);

#define mono_hazard_pointer_set(hp,i,v)	\
	do { g_assert ((i) >= 0 && (i) < HAZARD_POINTER_COUNT); \
		(hp)->hazard_pointers [(i)] = (v); \
		mono_memory_write_barrier (); \
	} while (0)

#define mono_hazard_pointer_get_val(hp,i)	\
	((hp)->hazard_pointers [(i)])

#define mono_hazard_pointer_clear(hp,i)	\
	do { g_assert ((i) >= 0 && (i) < HAZARD_POINTER_COUNT); \
		(hp)->hazard_pointers [(i)] = NULL; \
	} while (0)

void mono_thread_attach (void);

void mono_thread_smr_init (void);
void mono_thread_hazardous_print_stats (void);

#endif
