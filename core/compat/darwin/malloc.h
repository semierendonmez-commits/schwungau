/* glibc's <malloc.h>: only malloc_trim()/mallopt() are used, both advisory. */
#pragma once
#include <stdlib.h>
#include <malloc/malloc.h>
static inline int malloc_trim(size_t pad) { (void)pad; return 0; }
static inline int mallopt(int p, int v) { (void)p; (void)v; return 0; }
#ifndef M_TRIM_THRESHOLD
#define M_TRIM_THRESHOLD (-1)
#define M_MMAP_THRESHOLD (-3)
#endif
