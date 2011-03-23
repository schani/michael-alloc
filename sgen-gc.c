#include <unistd.h>

#include "mono-mmap.h"

#include "sgen-gc.h"

static size_t total_alloc = 0;

/*
 * Allocate a big chunk of memory from the OS (usually 64KB to several megabytes).
 * This must not require any lock.
 */
void*
mono_sgen_alloc_os_memory (size_t size, int activate)
{
	size_t pagesize = getpagesize ();
	void *ptr;
	unsigned long prot_flags = activate? MONO_MMAP_READ|MONO_MMAP_WRITE: MONO_MMAP_NONE;

	size += pagesize - 1;
	size &= ~(pagesize - 1);
	ptr = mono_valloc (0, size, prot_flags);
	/* FIXME: CAS */
	total_alloc += size;
	return ptr;
}

/*
 * Free the memory returned by mono_sgen_alloc_os_memory (), returning it to the OS.
 */
void
mono_sgen_free_os_memory (void *addr, size_t size)
{
	size_t pagesize = getpagesize ();

	mono_vfree (addr, size);

	size += pagesize - 1;
	size &= ~(pagesize - 1);
	/* FIXME: CAS */
	total_alloc -= size;
}

void*
mono_sgen_alloc_os_memory_aligned (mword size, mword alignment, gboolean activate)
{
	/* Allocate twice the memory to be able to put the block on an aligned address */
	char *mem = mono_sgen_alloc_os_memory (size + alignment, activate);
	char *aligned;

	g_assert (mem);

	aligned = (char*)((mword)(mem + (alignment - 1)) & ~(alignment - 1));
	g_assert (aligned >= mem && aligned + size <= mem + size + alignment && !((mword)aligned & (alignment - 1)));

	if (aligned > mem)
		mono_sgen_free_os_memory (mem, aligned - mem);
	if (aligned + size < mem + size + alignment)
		mono_sgen_free_os_memory (aligned + size, (mem + size + alignment) - (aligned + size));

	return aligned;
}
