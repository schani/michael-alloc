/*
 * lock-free-alloc.h: Lock free allocator.
 *
 * (C) Copyright 2011 Novell, Inc
 */

#ifndef __MONO_LOCKFREEALLOC_H__
#define __MONO_LOCKFREEALLOC_H__

#include "queue.h"

typedef struct {
	MonoLockFreeQueue partial;
	unsigned int slot_size;
} MonoLockFreeAllocSizeClass;

struct _MonoLockFreeAllocDescriptor;

typedef struct {
	struct _MonoLockFreeAllocDescriptor *active;
	MonoLockFreeAllocSizeClass *sc;
} MonoLockFreeAllocator;

#endif
