// Uses plain malloc/free so the same binary runs against glibc or qalloc via LD_PRELOAD.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <mutex>
#include <condition_variable>
#include <deque>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) { return std::chrono::duration<double>(b - a).count(); }
static volatile uintptr_t sink;

// 1) tight alloc/free of a fixed size (pure fast path)
static double fixed_size(size_t sz, long iters) {
    auto t0 = clk::now();
    for (long i = 0; i < iters; i++) { void* p = malloc(sz); sink = (uintptr_t)p; *(char*)p = 1; free(p); }
    return secs(t0, clk::now()) / iters * 1e9;
}

// 2) random sizes, random slot replacement, fixed working set per thread
static void churn(int seed, long iters, size_t maxsz, int slots) {
    std::mt19937_64 rng(seed);
    std::vector<void*> v(slots, nullptr);
    for (long i = 0; i < iters; i++) {
        size_t k = rng() % slots;
        free(v[k]);
        size_t n = 16 + rng() % maxsz;
        v[k] = malloc(n);
        memset(v[k], 1, n < 64 ? n : 64);
    }
    for (void* p : v) free(p);
}
static double churn_mt(int threads, long iters, size_t maxsz) {
    auto t0 = clk::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++) ts.emplace_back(churn, t + 1, iters, maxsz, 4096);
    for (auto& t : ts) t.join();
    return secs(t0, clk::now()) / (double(iters) * threads) * 1e9;
}

// 3) producer -> consumer: every object is freed by a different thread than allocated it
static double producer_consumer(int pairs, long iters) {
    struct Q { std::mutex m; std::condition_variable cv; std::deque<void*> d; bool done = false; };
    std::vector<Q> qs(pairs);
    auto t0 = clk::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < pairs; i++) {
        ts.emplace_back([&, i] {  // consumer
            Q& q = qs[i];
            std::deque<void*> local;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(q.m);
                    q.cv.wait(lk, [&] { return !q.d.empty() || q.done; });
                    local.swap(q.d);
                    if (local.empty() && q.done) return;
                }
                for (void* p : local) free(p);
                local.clear();
            }
        });
        ts.emplace_back([&, i] {  // producer; batches of 64 keep queue-lock cost out of the picture
            Q& q = qs[i];
            std::vector<void*> b;
            for (long n = 0; n < iters; n++) {
                void* p = malloc(48 + (n & 255)); *(char*)p = 1; b.push_back(p);
                if (b.size() == 64 || n + 1 == iters) {
                    { std::lock_guard<std::mutex> lk(q.m); for (void* x : b) q.d.push_back(x); }
                    q.cv.notify_one(); b.clear();
                }
            }
            { std::lock_guard<std::mutex> lk(q.m); q.done = true; }
            q.cv.notify_one();
        });
    }
    for (auto& t : ts) t.join();
    return secs(t0, clk::now()) / (double(iters) * pairs) * 1e9;
}

int main() {
    printf("%-34s %10s\n", "benchmark", "ns/op");
    printf("%-34s %10.1f\n", "fixed 16B alloc+free (1T)", fixed_size(16, 50000000));
    printf("%-34s %10.1f\n", "fixed 256B alloc+free (1T)", fixed_size(256, 50000000));
    printf("%-34s %10.1f\n", "churn 16..1K (1T)", churn_mt(1, 5000000, 1024));
    printf("%-34s %10.1f\n", "churn 16..1K (4T)", churn_mt(4, 5000000, 1024));
    printf("%-34s %10.1f\n", "churn 16..1K (8T)", churn_mt(8, 5000000, 1024));
    printf("%-34s %10.1f\n", "churn 16..16K (4T)", churn_mt(4, 2000000, 16384));
    printf("%-34s %10.1f\n", "producer/consumer (2 pairs)", producer_consumer(2, 3000000));
    printf("%-34s %10.1f\n", "producer/consumer (4 pairs)", producer_consumer(4, 3000000));
    return 0;
}
