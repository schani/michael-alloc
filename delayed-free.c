#include <glib.h>

#include "metadata.h"
#include "mono-mmap.h"
#include "atomic.h"
#include "mono-membar.h"

#include "delayed-free.h"

enum {
	STATE_FREE,
	STATE_USED,
	STATE_BUSY
};

typedef struct {
	gint32 state;
	MonoDelayedFreeItem item;
} Entry;

typedef struct _Chunk Chunk;
struct _Chunk {
	Chunk *next;
	gint32 num_entries;
	Entry entries [MONO_ZERO_LEN_ARRAY];
};

static volatile gint32 num_used_entries;
static Chunk *chunk_list;

static long long stat_delayed_free_sleep;

static Chunk*
alloc_chunk (void)
{
	int size = mono_pagesize ();
	int num_entries = (size - (sizeof (Chunk) - sizeof (Entry) * MONO_ZERO_LEN_ARRAY)) / sizeof (Entry);
	Chunk *chunk = mono_valloc (0, size, MONO_MMAP_READ | MONO_MMAP_WRITE);
	chunk->num_entries = num_entries;
	return chunk;
}

void
free_chunk (Chunk *chunk)
{
	mono_vfree (chunk, mono_pagesize ());
}

static Entry*
get_entry (int index)
{
	Chunk *chunk;

	g_assert (index >= 0);

	if (!chunk_list) {
		chunk = alloc_chunk ();
		if (InterlockedCompareExchangePointer ((volatile gpointer *)&chunk_list, chunk, NULL) != NULL)
			free_chunk (chunk);
	}

	chunk = chunk_list;
	g_assert (chunk);

	while (index >= chunk->num_entries) {
		Chunk *next = chunk->next;
		if (!next) {
			next = alloc_chunk ();
			if (InterlockedCompareExchangePointer ((volatile gpointer *) &chunk->next, next, NULL) != NULL) {
				free_chunk (next);
				next = chunk->next;
				g_assert (next);
			}
		}
		index -= chunk->num_entries;
		chunk = next;
	}

	return &chunk->entries [index];
}

void
mono_delayed_free_push (MonoDelayedFreeItem item)
{
	int index = InterlockedIncrement (&num_used_entries) - 1;
	Entry *entry = get_entry (index);

	while (InterlockedCompareExchange (&entry->state, STATE_BUSY, STATE_FREE) != STATE_FREE) {
		usleep (50);
		++stat_delayed_free_sleep;
	}

	entry->item = item;
	mono_memory_write_barrier ();
	entry->state = STATE_USED;
}

gboolean
mono_delayed_free_pop (MonoDelayedFreeItem *item)
{
	int index;
	Entry *entry;

	do {
		index = num_used_entries;
		if (index == 0)
			return FALSE;
	} while (InterlockedCompareExchange (&num_used_entries, index - 1, index) != index);

	--index;

	entry = get_entry (index);

	while (InterlockedCompareExchange (&entry->state, STATE_BUSY, STATE_USED) != STATE_USED) {
		usleep (40);
		++stat_delayed_free_sleep;
	}

	*item = entry->item;
	entry->state = STATE_FREE;

	return TRUE;
}

void
mono_delayed_free_print_stats (void)
{
	g_print ("delayed free sleep: %lld\n", stat_delayed_free_sleep);
}
