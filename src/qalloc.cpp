// qalloc: thread-caching allocator.
//
//  malloc fast path:   TLS ThreadCache -> per-size-class intrusive free list (no locks, no atomics)
//  refill / overflow:  per-size-class Central list (spinlock, batch transfers of N objects)
//  Central refill:     bump-carve objects out of a 256 KiB, 256 KiB-aligned "span"
//  Span supply:        PageHeap carves spans out of 4 MiB mmap'd chunks
//  > 32 KiB:           direct mmap, header at the aligned base, user pointer = base + offset
//
// free(p) needs no per-object header: p & ~(kSpanSize-1) is the span/large header, which
// records the size class (or marks the block large). Remote frees (freed by a thread other
// than the allocating one) are fine: any object may go into any thread's bin for its class.

#include "qalloc.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <atomic>

#if defined(__GNUC__)
#define QA_LIKELY(x) __builtin_expect(!!(x), 1)
#define QA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define QA_LIKELY(x) (x)
#define QA_UNLIKELY(x) (x)
#endif

#if defined(__x86_64__) || defined(__i386__)
#define QA_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define QA_PAUSE() __asm__ __volatile__("yield")
#else
#define QA_PAUSE() ((void)0)
#endif

namespace {

constexpr size_t kPage = 4096;
constexpr size_t kSpanSize = 256 * 1024;       // alignment + size of every span
constexpr size_t kSpanMask = ~(kSpanSize - 1);
constexpr size_t kHdrSize = kPage;             // header area at the start of each span
constexpr size_t kMaxSmall = 32 * 1024;
constexpr size_t kChunkSize = 4 * 1024 * 1024; // PageHeap grows in chunks of 16 spans
constexpr size_t kMaxAlign = kSpanSize / 2;
constexpr int kNumClasses = 40;
constexpr unsigned kLargeCls = 0xFFFFu;
constexpr uint32_t kMagic = 0x51414C43u;       // "QALC"

// ---------------------------------------------------------------- size classes
constexpr size_t kLookupN = (kMaxSmall >> 4) + 1;

struct SizeMap {
    uint16_t size[kNumClasses];
    uint8_t lookup[kLookupN];  // (bytes+15)/16 -> class
};

constexpr SizeMap make_map() {
    SizeMap m{};
    int n = 0;
    for (int s = 16; s <= 128; s += 16) m.size[n++] = (uint16_t)s;          // 16..128 step 16
    for (int p = 128; p < (int)kMaxSmall; p *= 2)                           // 4 classes / doubling
        for (int k = 1; k <= 4; k++) m.size[n++] = (uint16_t)(p + k * (p / 4));
    int c = 0;
    for (size_t i = 0; i < kLookupN; i++) {
        int sz = (int)(i * 16);
        while ((int)m.size[c] < sz) c++;
        m.lookup[i] = (uint8_t)c;
    }
    return m;
}
constexpr SizeMap kMap = make_map();
static_assert(kMap.size[kNumClasses - 1] == kMaxSmall, "class table must end at kMaxSmall");

inline unsigned class_of(size_t bytes) { return kMap.lookup[(bytes + 15) >> 4]; }

// ---------------------------------------------------------------- headers / stats
struct Hdr {  // lives at the (kSpanSize-aligned) base of every span and large block
    uint32_t magic;
    uint32_t cls;     // size class, or kLargeCls
    uint32_t arena;   // small spans: index of the owning arena
    size_t mapped;   // large only: bytes mapped
    size_t offset;    // large only: user pointer = base + offset
};

std::atomic<size_t> g_mapped{0}, g_large_live{0}, g_spans{0}, g_large_allocs{0};

// ---------------------------------------------------------------- locks
struct SpinLock {
    std::atomic<bool> f{false};
    void lock() {
        int spins = 0;
        while (f.exchange(true, std::memory_order_acquire)) {
            while (f.load(std::memory_order_relaxed)) {
                if (++spins < 128) QA_PAUSE();
                else { spins = 0; sched_yield(); }
            }
        }
    }
    void unlock() { f.store(false, std::memory_order_release); }
};

// ---------------------------------------------------------------- OS layer
void* os_map(size_t len) {
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

// size must be a multiple of kPage. Result is aligned to kSpanSize.
void* os_map_aligned(size_t size) {
    size_t len = size + kSpanSize;
    void* p = os_map(len);
    if (!p) return nullptr;
    uintptr_t a = (uintptr_t)p;
    uintptr_t al = (a + kSpanSize - 1) & kSpanMask;
    if (al > a) munmap(p, al - a);
    uintptr_t end = a + len, want_end = al + size;
    if (end > want_end) munmap((void*)want_end, end - want_end);
    g_mapped.fetch_add(size, std::memory_order_relaxed);
    return (void*)al;
}

// ---------------------------------------------------------------- metadata (never freed)
SpinLock g_meta_lock;
char *g_meta_cur, *g_meta_end;

void* meta_alloc(size_t sz) {
    sz = (sz + 63) & ~size_t(63);
    g_meta_lock.lock();
    if (g_meta_cur + sz > g_meta_end) {
        size_t chunk = 1 << 20;
        char* p = (char*)os_map(chunk);
        if (!p) { g_meta_lock.unlock(); return nullptr; }
        g_meta_cur = p; g_meta_end = p + chunk;
    }
    void* r = g_meta_cur;
    g_meta_cur += sz;
    g_meta_lock.unlock();
    return r;
}

// ---------------------------------------------------------------- arenas
// An arena is an independent contention domain: its own page heap (span supply) and its own
// central free list per size class, each under its own lock. Threads are bound to an arena
// round-robin when they get a ThreadCache. Every span is stamped with its owner, and objects
// are always returned to the owner's central lists (even if freed by a thread bound elsewhere),
// so an arena's memory stays within that arena.
struct FreeObj { FreeObj* next; };

struct alignas(64) Central {
    SpinLock lock;
    FreeObj* head;
    char* cur;  // bump region of the current span
    char* end;
};

struct alignas(64) Arena {
    SpinLock page_lock;       // guards the chunk bump pointers below
    char *page_cur, *page_end;
    Central central[kNumClasses];
};

constexpr unsigned kMaxArenas = 64;
Arena g_arenas[kMaxArenas];               // zero-initialised .bss: no constructors needed
unsigned g_num_arenas = 1;                // fixed once by global_init() before first use
std::atomic<unsigned> g_next_arena{0};

unsigned detect_arena_count() {
    unsigned n = 0;
    if (const char* e = getenv("QA_ARENAS")) {  // override, e.g. QA_ARENAS=1 for an A/B test
        for (; *e >= '0' && *e <= '9'; e++) n = n * 10 + (unsigned)(*e - '0');
    }
    if (n == 0) {
        cpu_set_t set;
        n = sched_getaffinity(0, sizeof(set), &set) == 0 ? (unsigned)CPU_COUNT(&set) : 1;
    }
    return n < 1 ? 1 : (n > kMaxArenas ? kMaxArenas : n);
}

char* arena_get_span(Arena& a) {
    a.page_lock.lock();
    if (a.page_cur == a.page_end) {
        char* c = (char*)os_map_aligned(kChunkSize);
        if (!c) { a.page_lock.unlock(); return nullptr; }
        a.page_cur = c; a.page_end = c + kChunkSize;
    }
    char* s = a.page_cur;
    a.page_cur += kSpanSize;
    a.page_lock.unlock();
    g_spans.fetch_add(1, std::memory_order_relaxed);
    return s;
}

inline unsigned batch_for(unsigned cls) {
    unsigned b = (unsigned)(32768 / kMap.size[cls]);
    return b < 2 ? 2 : (b > 64 ? 64 : b);
}

// Pop up to n objects (as a list) for class cls. Returns count; *out = list head.
unsigned central_fetch(unsigned ai, unsigned cls, unsigned n, FreeObj** out) {
    Arena& arena = g_arenas[ai];
    Central& c = arena.central[cls];
    const size_t sz = kMap.size[cls];
    FreeObj* list = nullptr;
    unsigned got = 0;
    c.lock.lock();
    while (got < n && c.head) {
        FreeObj* o = c.head;
        c.head = o->next;
        o->next = list; list = o; ++got;
    }
    while (got < n) {
        if (c.cur + sz > c.end) {
            char* s = arena_get_span(arena);
            if (!s) break;
            Hdr* h = (Hdr*)s;
            h->magic = kMagic; h->cls = cls; h->arena = ai; h->mapped = 0; h->offset = kHdrSize;
            c.cur = s + kHdrSize; c.end = s + kSpanSize;
        }
        FreeObj* o = (FreeObj*)c.cur;
        c.cur += sz;
        o->next = list; list = o; ++got;
    }
    c.lock.unlock();
    *out = list;
    return got;
}

void central_release(unsigned ai, unsigned cls, FreeObj* head, FreeObj* tail) {
    Central& c = g_arenas[ai].central[cls];
    c.lock.lock();
    tail->next = c.head;
    c.head = head;
    c.lock.unlock();
}

// ---------------------------------------------------------------- thread cache
struct Bin { FreeObj* head; uint32_t count; uint32_t limit; };
struct alignas(64) ThreadCache {
    Bin bins[kNumClasses];
    ThreadCache* next_free;
    unsigned arena;  // arena this thread refills from
};

__thread ThreadCache* t_cache = nullptr;
pthread_key_t g_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
SpinLock g_cache_lock;
ThreadCache* g_cache_free;

// Detach n objects from a bin and return them to the central lists of their *owning* arenas.
// A bin can hold objects from several arenas (remote frees), so the batch is partitioned by
// owner. The common case (everything owned by one arena) takes a single-lock fast path.
void bin_release(unsigned cls, Bin& b, unsigned n) {
    FreeObj* o = b.head;
    const unsigned first = ((Hdr*)((uintptr_t)o & kSpanMask))->arena;
    FreeObj* head = o;
    FreeObj* tail = o;
    FreeObj* rest_h[kMaxArenas];
    FreeObj* rest_t[kMaxArenas];
    uint64_t mixed = 0;  // bitmask of arenas other than `first` seen in this batch
    unsigned i = 1;
    FreeObj* cur = o->next;
    for (; i < n; i++, cur = cur->next) {
        unsigned a = ((Hdr*)((uintptr_t)cur & kSpanMask))->arena;
        if (QA_LIKELY(a == first)) { tail->next = cur; tail = cur; continue; }
        if (!(mixed & (1ull << a))) { mixed |= 1ull << a; rest_h[a] = rest_t[a] = cur; }
        else { rest_t[a]->next = cur; rest_t[a] = cur; }
    }
    b.head = cur;
    b.count -= n;
    central_release(first, cls, head, tail);
    while (mixed) {
        unsigned a = (unsigned)__builtin_ctzll(mixed);
        mixed &= mixed - 1;
        central_release(a, cls, rest_h[a], rest_t[a]);
    }
}

void thread_exit(void* p) {
    ThreadCache* tc = (ThreadCache*)p;
    t_cache = nullptr;
    for (unsigned c = 0; c < (unsigned)kNumClasses; c++)
        if (tc->bins[c].count) bin_release(c, tc->bins[c], tc->bins[c].count);
    g_cache_lock.lock();
    tc->next_free = g_cache_free;
    g_cache_free = tc;
    g_cache_lock.unlock();
}

void global_init() {
    g_num_arenas = detect_arena_count();
    pthread_key_create(&g_key, thread_exit);
}


__attribute__((noinline)) ThreadCache* init_cache() {
    pthread_once(&g_key_once, global_init);
    g_cache_lock.lock();
    ThreadCache* tc = g_cache_free;
    if (tc) g_cache_free = tc->next_free;
    g_cache_lock.unlock();
    if (!tc) {
        tc = (ThreadCache*)meta_alloc(sizeof(ThreadCache));  // zeroed by mmap
        if (!tc) return nullptr;
        for (unsigned c = 0; c < (unsigned)kNumClasses; c++) tc->bins[c].limit = 2 * batch_for(c);
    }
    tc->arena = g_next_arena.fetch_add(1, std::memory_order_relaxed) % g_num_arenas;
    // Publish to TLS *before* pthread_setspecific: setspecific may itself call calloc.
    t_cache = tc;
    pthread_setspecific(g_key, tc);
    return tc;
}

// ---------------------------------------------------------------- small path
__attribute__((noinline)) void* alloc_slow(ThreadCache* tc, unsigned cls) {
    Bin& b = tc->bins[cls];
    FreeObj* list;
    unsigned got = central_fetch(tc->arena, cls, batch_for(cls), &list);
    if (!got) { errno = ENOMEM; return nullptr; }
    b.head = list->next;
    b.count = got - 1;
    return list;
}

inline void* alloc_class(unsigned cls) {
    ThreadCache* tc = t_cache;
    if (QA_UNLIKELY(!tc)) {
        tc = init_cache();
        if (!tc) { errno = ENOMEM; return nullptr; }
    }
    Bin& b = tc->bins[cls];
    FreeObj* o = b.head;
    if (QA_LIKELY(o != nullptr)) {
        b.head = o->next;
        b.count--;
        return o;
    }
    return alloc_slow(tc, cls);
}

__attribute__((noinline)) void free_slow(Bin& b, unsigned cls) {
    bin_release(cls, b, b.limit / 2);  // release one batch; leaves hysteresis
}

inline void free_small(void* p, unsigned cls) {
    ThreadCache* tc = t_cache;
    if (QA_UNLIKELY(!tc)) {
        tc = init_cache();
        if (!tc) return;  // leak rather than crash
    }
    Bin& b = tc->bins[cls];
    FreeObj* o = (FreeObj*)p;
    o->next = b.head;
    b.head = o;
    if (QA_UNLIKELY(++b.count > b.limit)) free_slow(b, cls);
}

// ---------------------------------------------------------------- large path
// align <= kMaxAlign, power of two. Fresh mmap => memory is zeroed.
void* alloc_large(size_t size, size_t align) {
    size_t off = align > kHdrSize ? align : kHdrSize;
    if (size > (SIZE_MAX >> 2)) { errno = ENOMEM; return nullptr; }
    size_t total = (off + size + kPage - 1) & ~(kPage - 1);
    char* base = (char*)os_map_aligned(total);
    if (!base) { errno = ENOMEM; return nullptr; }
    Hdr* h = (Hdr*)base;
    h->magic = kMagic; h->cls = kLargeCls; h->mapped = total; h->offset = off;
    g_large_live.fetch_add(total, std::memory_order_relaxed);
    g_large_allocs.fetch_add(1, std::memory_order_relaxed);
    return base + off;
}

void free_large(Hdr* h) {
    size_t m = h->mapped;
    g_large_live.fetch_sub(m, std::memory_order_relaxed);
    g_mapped.fetch_sub(m, std::memory_order_relaxed);
    munmap(h, m);
}

inline Hdr* hdr_of(void* p) { return (Hdr*)((uintptr_t)p & kSpanMask); }

}  // namespace

// ================================================================= public API
extern "C" {

void* qa_malloc(size_t size) {
    if (QA_LIKELY(size <= kMaxSmall)) return alloc_class(class_of(size));
    return alloc_large(size, kHdrSize);
}

void qa_free(void* p) {
    if (QA_UNLIKELY(!p)) return;
    Hdr* h = hdr_of(p);
    if (QA_LIKELY(h->cls != kLargeCls)) free_small(p, h->cls);
    else free_large(h);
}

size_t qa_usable_size(void* p) {
    if (!p) return 0;
    Hdr* h = hdr_of(p);
    if (h->cls != kLargeCls) {
        // object start is at base + kHdrSize + i*size, so remaining bytes to end of object:
        size_t sz = kMap.size[h->cls];
        size_t rel = ((uintptr_t)p - (uintptr_t)h - kHdrSize) % sz;
        return sz - rel;
    }
    return h->mapped - ((uintptr_t)p - (uintptr_t)h);
}

void* qa_calloc(size_t n, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(n, size, &total)) { errno = ENOMEM; return nullptr; }
    if (total > kMaxSmall) return alloc_large(total, kHdrSize);  // fresh mmap: already zero
    void* p = qa_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* qa_realloc(void* p, size_t size) {
    if (!p) return qa_malloc(size);
    if (size == 0) { qa_free(p); return nullptr; }
    size_t usable = qa_usable_size(p);
    // Keep in place if it fits and we are not wasting more than half the block.
    if (size <= usable && (size >= usable / 2 || usable <= 64)) return p;
    void* q = qa_malloc(size);
    if (!q) return nullptr;
    memcpy(q, p, size < usable ? size : usable);
    qa_free(p);
    return q;
}

void* qa_memalign(size_t align, size_t size) {
    if (align <= 16) return qa_malloc(size);
    if (align & (align - 1)) { errno = EINVAL; return nullptr; }
    if (align > kMaxAlign) { errno = ENOMEM; return nullptr; }
    if (align <= kHdrSize && size <= kMaxSmall) {
        size_t need = size > align ? size : align;
        // Objects sit at base + kHdrSize + i*classSize; with kHdrSize % align == 0, any class
        // whose size is a multiple of align yields align-aligned objects.
        for (unsigned c = class_of(need); c < (unsigned)kNumClasses; c++)
            if (kMap.size[c] % align == 0) return alloc_class(c);
    }
    return alloc_large(size, align);
}

int qa_posix_memalign(void** out, size_t align, size_t size) {
    if (align < sizeof(void*) || (align & (align - 1))) return EINVAL;
    int saved = errno;
    void* p = qa_memalign(align, size);
    if (!p) { int e = errno; errno = saved; return e ? e : ENOMEM; }
    errno = saved;
    *out = p;
    return 0;
}

void qa_get_stats(qa_stats* s) {
    s->mapped_bytes = g_mapped.load(std::memory_order_relaxed);
    s->large_live_bytes = g_large_live.load(std::memory_order_relaxed);
    s->spans_carved = g_spans.load(std::memory_order_relaxed);
    s->large_allocs = g_large_allocs.load(std::memory_order_relaxed);
    s->num_arenas = g_num_arenas;
}

}  // extern "C"
