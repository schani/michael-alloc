#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "hazard.h"
#include "atomic.h"
#include "lock-free-alloc.h"
#include "mono-linked-list-set.h"

#ifdef TEST_ALLOC
#define USE_SMR

#define ACTION_BUFFER_SIZE	16

typedef struct {
	int action;
	int index;
	gpointer p;
} ThreadAction;

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;

	ThreadAction action_buffer [ACTION_BUFFER_SIZE];
	int next_action_index;
} ThreadData;

#endif

#ifdef TEST_QUEUE
#define USE_SMR

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;

	gint32 next_enqueue_counter;
	gint32 last_dequeue_counter;
} ThreadData;
#endif

#ifdef TEST_DELAYED_FREE
typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;
} ThreadData;
#endif

#ifdef TEST_LLS
#define USE_SMR

typedef struct {
	pthread_t thread;
	int increment;
	volatile gboolean have_attached;
} ThreadData;
#endif

#define NUM_THREADS	4

static ThreadData thread_datas [NUM_THREADS];

static void
attach_and_wait_for_threads_to_attach (ThreadData *data)
{
	int i;

	mono_thread_attach ();
	data->have_attached = TRUE;

 retry:
	for (i = 0; i < NUM_THREADS; ++i) {
		if (!thread_datas [i].have_attached) {
			usleep (5000);
			goto retry;
		}
	}
}

#ifdef TEST_ALLOC

#define TEST_SIZE	64

static MonoLockFreeAllocSizeClass test_sc;
static MonoLockFreeAllocator test_heap;

static void
init_heap (void)
{
	mono_lock_free_allocator_init_size_class (&test_sc, TEST_SIZE);
	mono_lock_free_allocator_init_allocator (&test_heap, &test_sc);
}

enum {
	ACTION_NONE,
	ACTION_ALLOC,
	ACTION_FREE
};

#define NUM_ENTRIES	1024
#define NUM_ITERATIONS	100000000

static gpointer entries [NUM_ENTRIES];

static volatile guint64 atomic_test;

static void
log_action (ThreadData *data, int action, int index, gpointer p)
{
	data->action_buffer [data->next_action_index].action = action;
	data->action_buffer [data->next_action_index].index = index;
	data->action_buffer [data->next_action_index].p = p;

	data->next_action_index = (data->next_action_index + 1) % ACTION_BUFFER_SIZE;
}

static void
dump_action_logs (void)
{
	int i, j;

	for (i = 0; i < NUM_THREADS; ++i) {
		g_print ("action log for thread %d:\n\n", i);

		j = thread_datas [i].next_action_index;
		do {
			ThreadAction *action = &thread_datas [i].action_buffer [j];
			switch (action->action) {
			case ACTION_NONE:
				break;

			case ACTION_ALLOC:
				g_print ("%6d %p alloc\n", action->index, action->p);
				break;

			case ACTION_FREE:
				g_print ("%6d %p free\n", action->index, action->p);
				break;

			default:
				g_assert_not_reached ();
			}

			j = (j + 1) % ACTION_BUFFER_SIZE;
		} while (j != thread_datas [i].next_action_index);

		g_print ("\n\n");
	}
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int i, index;

	attach_and_wait_for_threads_to_attach (data);

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		gpointer p;
	retry:
		p = entries [index];
		if (p) {
			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], NULL, p) != p)
				goto retry;
			g_assert (*(int*)p == index << 10);
			*(int*)p = -1;
			mono_lock_free_free (p);

			log_action (data, ACTION_FREE, index, p);
		} else {
			p = mono_lock_free_alloc (&test_heap);

			/*
			int j;

			for (j = 0; j < NUM_ENTRIES; ++j)
				g_assert (entries [j] != p);
			*/

			*(int*)p = index << 10;

			log_action (data, ACTION_ALLOC, index, p);

			if (InterlockedCompareExchangePointer ((gpointer * volatile)&entries [index], p, NULL) != NULL) {
				//g_print ("immediate free %p\n", p);
				*(int*)p = -1;
				mono_lock_free_free (p);

				log_action (data, ACTION_FREE, index, p);

				goto retry;
			}
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;

		guint64 a = atomic_test;
		g_assert ((a & 0xffffffff) == (a >> 32));
		guint64 new_a = (index | ((guint64)index << 32));
		atomic64_cmpxchg ((volatile gint64*)&atomic_test, a, new_a);

		if (i % (NUM_ITERATIONS / 20) == 0)
			g_print ("thread %d: %d\n", increment, i);
	}

	return NULL;
}

static void
test_init (void)
{
	init_heap ();
	mono_lock_free_alloc (&test_heap);
}

static gboolean
test_finish (void)
{
	if (mono_lock_free_allocator_check_consistency (&test_heap)) {
		g_print ("heap consistent\n");
		return TRUE;
	}
	return FALSE;
}

#endif

#ifdef TEST_QUEUE

#define NUM_ENTRIES	16
#define NUM_ITERATIONS	10000000

typedef struct _TableEntry TableEntry;

typedef struct {
	MonoLockFreeQueueNode node;
	TableEntry *table_entry;
	ThreadData *thread_data;
	gint32 counter;
} QueueEntry;

struct _TableEntry {
	gboolean mmap;
	QueueEntry *queue_entry;
};

static MonoLockFreeQueue queue;
static TableEntry entries [NUM_ENTRIES];

static QueueEntry*
alloc_entry (TableEntry *e, ThreadData *thread_data)
{
	QueueEntry *qe;

	if (e->mmap)
		qe = mmap (NULL, getpagesize (), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	else
		qe = g_malloc0 (sizeof (QueueEntry));

	mono_lock_free_queue_node_init (&qe->node, FALSE);

	qe->thread_data = thread_data;
	qe->counter = thread_data->next_enqueue_counter++;

	return qe;
}

static void
free_entry_memory (QueueEntry *qe, gboolean mmap)
{
	if (mmap)
		munmap (qe, getpagesize ());
	else
		g_free (qe);
}

static ThreadData thread_datas [NUM_THREADS];

static void
free_entry (void *data)
{
	QueueEntry *e = data;
	g_assert (e->table_entry->queue_entry == e);
	e->table_entry->queue_entry = NULL;
	mono_lock_free_queue_node_free (&e->node);
	free_entry_memory (e, e->table_entry->mmap);
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int index;
	int i;

	attach_and_wait_for_threads_to_attach (data);

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		TableEntry *e = &entries [index];

		if (e->queue_entry) {
			QueueEntry *qe = (QueueEntry*)mono_lock_free_queue_dequeue (&queue);
			if (qe) {
				if (qe->thread_data == data) {
					g_assert (qe->counter > data->last_dequeue_counter);
					data->last_dequeue_counter = qe->counter;
				}

				/*
				 * Calling free_entry() directly here
				 * effectively disables hazardous
				 * pointers.  The test will then crash
				 * sooner or later.
				 */
				mono_thread_hazardous_free_or_queue (qe, free_entry);
				//free_entry (qe);
			}
		} else {
			QueueEntry *qe = alloc_entry (e, data);
			qe->table_entry = e;
			if (InterlockedCompareExchangePointer ((gpointer volatile*)&e->queue_entry, qe, NULL) == NULL) {
				mono_lock_free_queue_enqueue (&queue, &qe->node);
			} else {
				qe->table_entry = NULL;
				free_entry_memory (qe, e->mmap);
			}
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	return NULL;
}

static void
test_init (void)
{
	int i;

	mono_lock_free_queue_init (&queue);

	/*
	for (i = 0; i < NUM_ENTRIES; i += 97)
		entries [i].mmap = TRUE;
	*/

	for (i = 0; i < NUM_THREADS; ++i)
		thread_datas [i].last_dequeue_counter = -1;
}

static gboolean
test_finish (void)
{
	QueueEntry *qe;
	int i;

	while ((qe = (QueueEntry*)mono_lock_free_queue_dequeue (&queue)))
		free_entry (qe);

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (!entries [i].queue_entry);

	return TRUE;
}

#endif

#ifdef TEST_DELAYED_FREE
#define NUM_ENTRIES	32768
#define NUM_ITERATIONS	1000000

static gint32 entries [NUM_ENTRIES];

static void
free_func (gpointer data)
{
	int i = (long)data;

	if (InterlockedCompareExchange (&entries [i], 0, 1) != 1)
		g_assert_not_reached ();
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int index, i;

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		if (InterlockedCompareExchange (&entries [index], 1, 0) == 0) {
			MonoDelayedFreeItem item = { (gpointer)(long)index, free_func };
			mono_delayed_free_push (item);
		} else {
			MonoDelayedFreeItem item;
			if (mono_delayed_free_pop (&item))
				item.free_func (item.p);
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	return NULL;
}

static void
test_init (void)
{
}

static gboolean
test_finish (void)
{
	int i;
	MonoDelayedFreeItem item;

	while (mono_delayed_free_pop (&item))
		item.free_func (item.p);

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (!entries [i]);

	return TRUE;
}
#endif

#ifdef TEST_LLS
enum {
	STATE_FREE,
	STATE_ALLOCING,
	STATE_FREEING,
	STATE_USED
};

#define NUM_ENTRIES	32
#define NUM_ITERATIONS	1000000

static gint32 entries [NUM_ENTRIES];

static MonoLinkedListSet list;

static void
free_node_func (void *_node)
{
	MonoLinkedListSetNode *node = _node;
	int index = node->key >> 2;
	g_assert (index >= 0 && index < NUM_ENTRIES);
	if (InterlockedCompareExchange (&entries [index], STATE_FREE, STATE_FREEING) != STATE_FREEING)
		g_assert_not_reached ();
	free (node);
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	MonoThreadHazardPointers *hp;
	int index, i;
	gboolean result;

	attach_and_wait_for_threads_to_attach (data);

	hp = mono_hazard_pointer_get ();

	index = 0;
	for (i = 0; i < NUM_ITERATIONS; ++i) {
		gint32 state = entries [index];

		if (state == STATE_FREE) {
			if (InterlockedCompareExchange (&entries [index], STATE_ALLOCING, STATE_FREE) == STATE_FREE) {
				MonoLinkedListSetNode *node = malloc (sizeof (MonoLinkedListSetNode));
				node->key = index << 2;

				result = mono_lls_insert (&list, hp, node);
				g_assert (result);

				if (InterlockedCompareExchange (&entries [index], STATE_USED, STATE_ALLOCING) != STATE_ALLOCING)
					g_assert_not_reached ();

				mono_hazard_pointer_clear (hp, 0);
				mono_hazard_pointer_clear (hp, 1);
				mono_hazard_pointer_clear (hp, 2);
			}
		} else if (state == STATE_USED) {
			if (InterlockedCompareExchange (&entries [index], STATE_FREEING, STATE_USED) == STATE_USED) {
				MonoLinkedListSetNode *node;

				result = mono_lls_find (&list, hp, index << 2);
				g_assert (result);

				node = mono_hazard_pointer_get_val (hp, 1);
				g_assert (node->key == index << 2);

				mono_hazard_pointer_clear (hp, 0);
				mono_hazard_pointer_clear (hp, 1);
				mono_hazard_pointer_clear (hp, 2);

				result = mono_lls_remove (&list, hp, node);
				g_assert (result);

				mono_hazard_pointer_clear (hp, 0);
				mono_hazard_pointer_clear (hp, 1);
				mono_hazard_pointer_clear (hp, 2);
			}
		} else {
			mono_thread_hazardous_try_free_all ();
		}

		index += increment;
		while (index >= NUM_ENTRIES)
			index -= NUM_ENTRIES;
	}

	return NULL;
}

static void
test_init (void)
{
	mono_lls_init (&list, free_node_func);
}

static gboolean
test_finish (void)
{
	MonoLinkedListSetNode *node;
	int i;

	MONO_LLS_FOREACH ((&list), node)
		int index = node->key >> 2;
		g_assert (index >= 0 && index < NUM_ENTRIES);
		g_assert (entries [index] == STATE_USED);
		entries [index] = STATE_FREE;
	MONO_LLS_END_FOREACH

	for (i = 0; i < NUM_ENTRIES; ++i)
		g_assert (entries [i] == STATE_FREE);

	return TRUE;
}
#endif

int
main (void)
{
	int i;
	gboolean result;

#ifdef USE_SMR
	mono_thread_smr_init ();

	mono_thread_attach ();
#endif

	test_init ();

	thread_datas [0].increment = 1;
	if (NUM_THREADS >= 2)
		thread_datas [1].increment = 3;
	if (NUM_THREADS >= 3)
		thread_datas [2].increment = 5;
	if (NUM_THREADS >= 4)
		thread_datas [3].increment = 7;

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_create (&thread_datas [i].thread, NULL, thread_func, &thread_datas [i]);

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_join (thread_datas [i].thread, NULL);

#ifdef USE_SMR
	mono_thread_hazardous_try_free_all ();
#endif

	result = test_finish ();

#ifdef USE_SMR
	mono_thread_hazardous_print_stats ();
#endif

	return result ? 0 : 1;
}
