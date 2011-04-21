#include <stdio.h>

#include "mono-mmap.h"

void*
mono_valloc (void *addr, size_t len, int prot)
{
	addr = mmap (addr, len, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (addr == (void*)-1) {
		fprintf (stderr, "mmap error: %m\n");
		return NULL;
	}
	return addr;
}

void
mono_vfree (void *addr, size_t len)
{
	munmap (addr, len);
}
