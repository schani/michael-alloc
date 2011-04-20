#ifndef __MONO_METADATA_H__
#define __MONO_METADATA_H__

#include <unistd.h>

#ifndef MONO_ZERO_LEN_ARRAY
#ifdef __GNUC__
#define MONO_ZERO_LEN_ARRAY 0
#else
#define MONO_ZERO_LEN_ARRAY 1
#endif
#endif

#define mono_pagesize	getpagesize

#endif
