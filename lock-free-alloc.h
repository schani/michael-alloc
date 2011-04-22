/*
 * lock-free-alloc.h: Lock free allocator.
 *
 * (C) Copyright 2011 Novell, Inc
 */

#ifndef __MONO_LOCKFREEALLOC_H__
#define __MONO_LOCKFREEALLOC_H__

#include <glib.h>

#include "lock-free-queue.h"

typedef struct {
	MonoLockFreeQueue partial;
	unsigned int slot_size;
} MonoLockFreeAllocSizeClass;

struct _MonoLockFreeAllocDescriptor;

typedef struct {
	struct _MonoLockFreeAllocDescriptor *active;
	MonoLockFreeAllocSizeClass *sc;
} MonoLockFreeAllocator;

void mono_lock_free_allocator_init_size_class (MonoLockFreeAllocSizeClass *sc, unsigned int slot_size);
void mono_lock_free_allocator_init_allocator (MonoLockFreeAllocator *heap, MonoLockFreeAllocSizeClass *sc);

gpointer mono_lock_free_alloc (MonoLockFreeAllocator *heap);
void mono_lock_free_free (gpointer ptr);

gboolean mono_lock_free_allocator_check_consistency (MonoLockFreeAllocator *heap);

#endif
