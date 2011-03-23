#ifndef __MONO_UTILS_MMAP_H__
#define __MONO_UTILS_MMAP_H__

#include <sys/mman.h>

#define mono_mprotect	mprotect
#define MONO_MMAP_NONE	PROT_NONE
#define MONO_MMAP_READ	PROT_READ
#define MONO_MMAP_WRITE	PROT_WRITE

void* mono_valloc (void *addr, size_t len, int prot);

void mono_vfree (void *addr, size_t len);

#endif
