#ifndef __MONO_METADATA_SGEN_GC_H__
#define __MONO_METADATA_SGEN_GC_H__

#include <glib.h>

typedef unsigned long mword;

void* mono_sgen_alloc_os_memory (size_t size, int activate);
void mono_sgen_free_os_memory (void *addr, size_t size);

void* mono_sgen_alloc_os_memory_aligned (mword size, mword alignment, gboolean activate);

#endif
