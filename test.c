#include <unistd.h>

#include "hazard.h"
#include "atomic.h"
#include "lock-free-alloc.h"

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

#define NUM_THREADS	4

enum {
	ACTION_NONE,
	ACTION_ALLOC,
	ACTION_FREE
};

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

static ThreadData thread_datas [NUM_THREADS];

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

static void
wait_for_threads_to_attach (void)
{
	int i;
 retry:
	for (i = 0; i < NUM_THREADS; ++i) {
		if (!thread_datas [i].have_attached) {
			usleep (5000);
			goto retry;
		}
	}
}

static void*
thread_func (void *_data)
{
	ThreadData *data = _data;
	int increment = data->increment;
	int i, index;

	mono_thread_attach ();
	data->have_attached = TRUE;
	wait_for_threads_to_attach ();

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

int
main (void)
{
	int i;

	mono_thread_hazardous_init ();

	mono_thread_attach ();

	init_heap ();
	mono_lock_free_alloc (&test_heap);

	thread_datas [0].increment = 1;
	if (NUM_THREADS >= 2)
		thread_datas [1].increment = 2;
	if (NUM_THREADS >= 3)
		thread_datas [2].increment = 3;
	if (NUM_THREADS >= 4)
		thread_datas [3].increment = 5;

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_create (&thread_datas [i].thread, NULL, thread_func, &thread_datas [i]);

	for (i = 0; i < NUM_THREADS; ++i)
		pthread_join (thread_datas [i].thread, NULL);

	mono_thread_hazardous_try_free_all ();

	mono_thread_hazardous_print_stats ();

	if (mono_lock_free_allocator_check_consistency (&test_heap)) {
		g_print ("heap consistent\n");
		return 0;
	}
	return 1;
}

#endif
