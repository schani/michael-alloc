#ifndef __FAKE_GLIB_H__
#define __FAKE_GLIB_H__

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define g_assert			assert
#define g_assert_not_reached()		(assert(0))

#define g_print				printf
#define g_warning			printf

static inline void* g_malloc0 (size_t s) {
	void *p = malloc (s);
	memset (p, 0, s);
	return p;
}
#define g_free	free

typedef void* gpointer;
typedef unsigned char guint8;
typedef int gboolean;
typedef int gint32;
typedef unsigned int guint32;
typedef unsigned long gulong;

#ifdef __x86_64__
typedef long gint64;
typedef unsigned long guint64;
#endif

#define TRUE	1
#define FALSE	0

#endif
