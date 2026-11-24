#include "qalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <random>
#include <atomic>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)

static void fill(void* p, size_t n, uint8_t v) { memset(p, v, n); }
static bool verify(const void* p, size_t n, uint8_t v) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) if (b[i] != v) return false;
    return true;
}

static void test_basic() {
    for (size_t n = 0; n <= 100000; n += (n < 2048 ? 1 : 257)) {
        void* p = qa_malloc(n);
        CHECK(p && ((uintptr_t)p % 16) == 0);
        CHECK(qa_usable_size(p) >= n);
        fill(p, n, 0x5A);
        CHECK(verify(p, n, 0x5A));
        qa_free(p);
    }
    qa_free(nullptr);
    puts("basic ok");
}

static void test_align() {
    for (size_t a = 16; a <= 65536; a *= 2)
        for (size_t n : {size_t(1), size_t(100), size_t(5000), size_t(40000), size_t(300000)}) {
            void* p = qa_memalign(a, n);
            CHECK(p && ((uintptr_t)p % a) == 0);
            CHECK(qa_usable_size(p) >= n);
            fill(p, n, 0x11);
            qa_free(p);
        }
    void* p;
    CHECK(qa_posix_memalign(&p, 64, 1000) == 0 && ((uintptr_t)p % 64) == 0);
    qa_free(p);
    CHECK(qa_posix_memalign(&p, 3, 8) != 0);
    puts("align ok");
}

static void test_calloc_realloc() {
    for (size_t n : {size_t(8), size_t(1000), size_t(30000), size_t(200000)}) {
        void* d = qa_malloc(n); fill(d, n, 0xAB); qa_free(d);  // dirty the memory first
        uint8_t* z = (uint8_t*)qa_calloc(n, 1);
        CHECK(verify(z, n, 0));
        qa_free(z);
    }
    CHECK(qa_calloc(SIZE_MAX / 2, 4) == nullptr);
    // realloc keeps contents across size-class and small<->large boundaries
    size_t sizes[] = {1, 20, 100, 700, 5000, 32768, 32769, 100000, 1000000, 50000, 4000, 30, 1};
    uint8_t* p = nullptr; size_t cur = 0;
    for (size_t s : sizes) {
        p = (uint8_t*)qa_realloc(p, s);
        CHECK(p);
        size_t keep = cur < s ? cur : s;
        CHECK(verify(p, keep, 0xC3));
        fill(p, s, 0xC3);
        cur = s;
    }
    qa_free(p);
    puts("calloc/realloc ok");
}

static void test_no_overlap() {
    std::mt19937 rng(42);
    struct A { uint8_t* p; size_t n; uint8_t v; };
    std::vector<A> live;
    for (int i = 0; i < 200000; i++) {
        if (live.size() > 5000 || ((rng() & 1) && !live.empty())) {
            size_t k = rng() % live.size();
            CHECK(verify(live[k].p, live[k].n, live[k].v));
            qa_free(live[k].p);
            live[k] = live.back(); live.pop_back();
        } else {
            size_t n = (rng() % 8 == 0) ? rng() % 70000 : rng() % 600;
            uint8_t* p = (uint8_t*)qa_malloc(n);
            CHECK(p);
            uint8_t v = (uint8_t)rng();
            fill(p, n, v);
            live.push_back({p, n, v});
        }
    }
    for (auto& a : live) { CHECK(verify(a.p, a.n, a.v)); qa_free(a.p); }
    puts("no-overlap ok");
}

static void test_threads_random() {
    std::vector<std::thread> ts;
    std::atomic<int> bad{0};
    for (int t = 0; t < 8; t++)
        ts.emplace_back([t, &bad] {
            std::mt19937 rng(t);
            std::vector<std::pair<uint8_t*, size_t>> live;
            for (int i = 0; i < 300000; i++) {
                if (live.size() > 2000 || ((rng() & 1) && !live.empty())) {
                    size_t k = rng() % live.size();
                    if (!verify(live[k].first, live[k].second, (uint8_t)(t + 1))) bad++;
                    qa_free(live[k].first);
                    live[k] = live.back(); live.pop_back();
                } else {
                    size_t n = rng() % 2000;
                    auto* p = (uint8_t*)qa_malloc(n);
                    fill(p, n, (uint8_t)(t + 1));
                    live.push_back({p, n});
                }
            }
            for (auto& a : live) qa_free(a.first);
        });
    for (auto& t : ts) t.join();
    CHECK(bad == 0);
    puts("threads-random ok");
}

// Producers allocate, consumers (different threads) free: remote frees + central lists.
static void test_cross_thread() {
    struct Q {
        std::mutex m; std::condition_variable cv;
        std::deque<std::pair<uint8_t*, size_t>> d; bool done = false;
    } q;
    std::atomic<int> bad{0};
    std::vector<std::thread> cs, ps;
    for (int c = 0; c < 4; c++)
        cs.emplace_back([&] {
            for (;;) {
                std::unique_lock<std::mutex> lk(q.m);
                q.cv.wait(lk, [&] { return !q.d.empty() || q.done; });
                if (q.d.empty()) return;
                auto it = q.d.front(); q.d.pop_front();
                lk.unlock();
                if (!verify(it.first, it.second, 0x77)) bad++;
                qa_free(it.first);
            }
        });
    for (int p = 0; p < 4; p++)
        ps.emplace_back([&, p] {
            std::mt19937 rng(p);
            for (int i = 0; i < 200000; i++) {
                size_t n = 1 + rng() % 1500;
                auto* b = (uint8_t*)qa_malloc(n);
                fill(b, n, 0x77);
                { std::lock_guard<std::mutex> lk(q.m); q.d.push_back({b, n}); }
                q.cv.notify_one();
            }
        });
    for (auto& t : ps) t.join();
    { std::lock_guard<std::mutex> lk(q.m); q.done = true; }
    q.cv.notify_all();
    for (auto& t : cs) t.join();
    CHECK(bad == 0);
    puts("cross-thread ok");
}

static void test_thread_churn_and_footprint() {
    qa_stats warm;
    for (int round = 0; round < 50; round++) {
        if (round == 25) qa_get_stats(&warm);  // arenas/caches populated by now
        std::vector<std::thread> ts;
        for (int t = 0; t < 40; t++)
            ts.emplace_back([] {
                void* p[256];
                for (int i = 0; i < 256; i++) p[i] = qa_malloc(8 + i * 7);
                for (int i = 0; i < 256; i++) qa_free(p[i]);
            });
        for (auto& t : ts) t.join();
    }
    qa_stats after; qa_get_stats(&after);
    // 2000 short-lived threads must recycle thread caches and objects, not leak spans: once warm
    // (each arena has its working spans), the second half of the run must not keep growing.
    // (Absolute footprint legitimately scales with arenas x active size classes.)
    CHECK(after.mapped_bytes - warm.mapped_bytes < 16u * 1024 * 1024);
    // steady-state alloc/free of one size must not grow the heap
    for (int i = 0; i < 3000000; i++) qa_free(qa_malloc(64));
    qa_stats end; qa_get_stats(&end);
    CHECK(end.mapped_bytes == after.mapped_bytes);
    // large blocks are returned to the OS
    void* big = qa_malloc(64u << 20);
    qa_stats mid; qa_get_stats(&mid);
    CHECK(mid.mapped_bytes >= after.mapped_bytes + (64u << 20));
    qa_free(big);
    qa_stats fin; qa_get_stats(&fin);
    CHECK(fin.mapped_bytes == after.mapped_bytes);
    puts("churn/footprint ok");
}

// Objects freed by a thread bound to a different arena must flow back to their owner arena, so a
// ping-pong (A allocs -> B frees -> A allocs ...) reaches a steady state instead of carving
// fresh spans every round.
static void test_remote_free_returns_to_owner() {
    qa_stats st0; qa_get_stats(&st0);
    std::mutex m; std::condition_variable cv;
    int turn = 0;  // 0: A's turn, 1: B's turn, 2: finished
    std::vector<void*> handoff;
    const int kRounds = 12, kObjs = 100000;
    size_t spans_after_first = 0, spans_end = 0;
    std::thread a([&] {
        for (int r = 0; r < kRounds; r++) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return turn == 0; });
            handoff.resize(kObjs);
            for (auto& p : handoff) p = qa_malloc(256);
            if (r == 0) { qa_stats s; qa_get_stats(&s); spans_after_first = s.spans_carved; }
            turn = 1; cv.notify_all();
        }
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return turn == 0; });
        qa_stats s; qa_get_stats(&s); spans_end = s.spans_carved;
        turn = 2; cv.notify_all();
    });
    std::thread b([&] {
        for (int r = 0; r < kRounds; r++) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return turn == 1; });
            for (void* p : handoff) qa_free(p);
            handoff.clear();
            turn = 0; cv.notify_all();
        }
    });
    a.join(); b.join();
    qa_stats st; qa_get_stats(&st);
    printf("  arenas=%u, spans after round 1=%zu, after %d rounds=%zu\n",
           st.num_arenas, spans_after_first - st0.spans_carved, kRounds, spans_end - st0.spans_carved);
    // Slack for objects parked in thread caches. Without owner-routing, when A and B sit in
    // different arenas this grows by ~100 spans every round.
    CHECK(spans_end - spans_after_first < 40);
    puts("remote-free-to-owner ok");
}

int main() {
    test_basic();
    test_align();
    test_calloc_realloc();
    test_no_overlap();
    test_threads_random();
    test_cross_thread();
    test_remote_free_returns_to_owner();
    test_thread_churn_and_footprint();
    puts("ALL TESTS PASSED");
    return 0;
}
