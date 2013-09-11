#include "fake-glib.h"

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

#ifdef FAILSAFE_DELAYED_FREE

#define NUM_ENTRIES	4

static Entry entries [NUM_ENTRIES];

void
mono_delayed_free_push (MonoDelayedFreeItem item)
{
	int i;

	for (i = 0; i < NUM_ENTRIES; ++i) {
		if (entries [i].state != STATE_FREE)
			continue;
		if (InterlockedCompareExchange (&entries [i].state, STATE_BUSY, STATE_FREE) == STATE_FREE) {
			mono_memory_write_barrier ();
			entries [i].item = item;
			mono_memory_write_barrier ();
			entries [i].state = STATE_USED;
			mono_memory_write_barrier ();
			return;
		}
	}
	g_assert_not_reached ();
}

gboolean
mono_delayed_free_pop (MonoDelayedFreeItem *item)
{
	int i;

	for (i = 0; i < NUM_ENTRIES; ++i) {
		if (entries [i].state != STATE_USED)
			continue;
		if (InterlockedCompareExchange (&entries [i].state, STATE_BUSY, STATE_USED) == STATE_USED) {
			mono_memory_barrier ();
			*item = entries [i].item;
			mono_memory_barrier ();
			entries [i].state = STATE_FREE;
			mono_memory_write_barrier ();
			return TRUE;
		}
	}
	return FALSE;
}

#else

typedef struct _Chunk Chunk;
struct _Chunk {
	Chunk *next;
	gint32 num_entries;
	Entry entries [MONO_ZERO_LEN_ARRAY];
};

static volatile gint32 num_used_entries;
static Chunk *chunk_list;

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
		mono_memory_write_barrier ();
		if (InterlockedCompareExchangePointer ((volatile gpointer *)&chunk_list, chunk, NULL) != NULL)
			free_chunk (chunk);
	}

	chunk = chunk_list;
	g_assert (chunk);

	while (index >= chunk->num_entries) {
		Chunk *next = chunk->next;
		if (!next) {
			next = alloc_chunk ();
			mono_memory_write_barrier ();
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
	int index, num_used;
	Entry *entry;

	do {
		index = InterlockedIncrement (&num_used_entries) - 1;
		entry = get_entry (index);
	} while (InterlockedCompareExchange (&entry->state, STATE_BUSY, STATE_FREE) != STATE_FREE);

	mono_memory_write_barrier ();

	entry->item = item;

	mono_memory_write_barrier ();

	entry->state = STATE_USED;

	mono_memory_barrier ();

	do {
		num_used = num_used_entries;
		if (num_used > index)
			break;
	} while (InterlockedCompareExchange (&num_used_entries, index + 1, num_used) != num_used);

	mono_memory_write_barrier ();
}

gboolean
mono_delayed_free_pop (MonoDelayedFreeItem *item)
{
	int index;
	Entry *entry;

	do {
		do {
			index = num_used_entries;
			if (index == 0)
				return FALSE;
		} while (InterlockedCompareExchange (&num_used_entries, index - 1, index) != index);

		entry = get_entry (index - 1);
	} while (InterlockedCompareExchange (&entry->state, STATE_BUSY, STATE_USED) != STATE_USED);

	/* Reading the item must happen before CASing the state. */
	mono_memory_barrier ();

	*item = entry->item;

	mono_memory_barrier ();

	entry->state = STATE_FREE;

	mono_memory_write_barrier ();

	return TRUE;
}

#endif
