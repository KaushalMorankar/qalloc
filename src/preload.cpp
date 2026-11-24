// LD_PRELOAD shim: replaces the process-wide malloc family with qalloc.
//   LD_PRELOAD=./libqalloc.so ./your_program
#include <stddef.h>
#include "qalloc.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT void* malloc(size_t n) { return qa_malloc(n); }
EXPORT void free(void* p) { qa_free(p); }
EXPORT void* calloc(size_t n, size_t s) { return qa_calloc(n, s); }
EXPORT void* realloc(void* p, size_t n) { return qa_realloc(p, n); }
EXPORT void* memalign(size_t a, size_t n) { return qa_memalign(a, n); }
EXPORT void* aligned_alloc(size_t a, size_t n) { return qa_memalign(a, n); }
EXPORT int posix_memalign(void** o, size_t a, size_t n) { return qa_posix_memalign(o, a, n); }
EXPORT void* valloc(size_t n) { return qa_memalign(4096, n); }
EXPORT void* pvalloc(size_t n) { return qa_memalign(4096, (n + 4095) & ~size_t(4095)); }
EXPORT size_t malloc_usable_size(void* p) { return qa_usable_size(p); }
