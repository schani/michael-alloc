#include "delayed-free.h"

static CRITICAL_SECTION delayed_free_table_mutex;
static GArray *delayed_free_table = NULL;

void
mono_delayed_free_push (MonoDelayedFreeItem item)
{
	EnterCriticalSection (&delayed_free_table_mutex);
	g_array_append_val (delayed_free_table, item);
	LeaveCriticalSection (&delayed_free_table_mutex);
}

gboolean
mono_delayed_free_pop (MonoDelayedFreeItem *item)
{
	gboolean result = FALSE;

	if (delayed_free_table->len == 0)
		return FALSE;

	EnterCriticalSection (&delayed_free_table_mutex);

	if (delayed_free_table->len > 0) {
		*item = g_array_index (delayed_free_table, MonoDelayedFreeItem, delayed_free_table->len - 1);
		g_array_remove_index_fast (delayed_free_table, delayed_free_table->len - 1);
		result = TRUE;
	}

	LeaveCriticalSection (&delayed_free_table_mutex);

	return result;
}

void
mono_delayed_free_init (void)
{
	pthread_mutex_init (&delayed_free_table_mutex, NULL);
	delayed_free_table = g_array_new (FALSE, FALSE, sizeof (MonoDelayedFreeItem));
}
