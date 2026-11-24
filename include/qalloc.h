// qalloc - a thread-caching, size-class based malloc for Linux (x86-64 / aarch64).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void*  qa_malloc(size_t size);
void   qa_free(void* p);
void*  qa_calloc(size_t n, size_t size);
void*  qa_realloc(void* p, size_t size);
void*  qa_memalign(size_t alignment, size_t size);
int    qa_posix_memalign(void** out, size_t alignment, size_t size);
size_t qa_usable_size(void* p);

typedef struct qa_stats {
    size_t mapped_bytes;      // total bytes currently mmap'd from the OS (spans + large blocks)
    size_t large_live_bytes;  // bytes in live large (> 32 KiB) blocks
    size_t spans_carved;      // number of 256 KiB spans handed to size classes
    size_t large_allocs;      // cumulative large allocations (each one is an mmap)
    unsigned num_arenas;      // arenas in use (QA_ARENAS env var overrides; default = #CPUs, max 64)
} qa_stats;

void qa_get_stats(qa_stats* out);

#ifdef __cplusplus
}
#endif
