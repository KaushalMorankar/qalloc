# qalloc — a thread-caching `malloc` for Linux

A from-scratch, drop-in replacement for the C allocator (~400 lines of C++17, no dependencies beyond libc/pthreads), in the style of tcmalloc. Usable as a library (`qa_malloc`/`qa_free`) or process-wide via `LD_PRELOAD`.

```
 malloc(n) ──► size class (O(1) table lookup, 40 classes, ≤12.5% internal waste)
              │
              ▼
   ┌──────────────────────┐  miss   ┌────────────────────────┐  empty  ┌───────────────────┐
   │ ThreadCache (TLS)    │ ──────► │ Central list per class │ ──────► │ PageHeap          │
   │ intrusive free list  │ ◄────── │ spinlock, batch xfer   │         │ 256 KiB spans cut │
   │ per class, no locks  │ overflow│ (2..64 objs / trip)    │         │ from 4 MiB mmaps  │
   └──────────────────────┘         └────────────────────────┘         └───────────────────┘
 > 32 KiB: direct mmap, header at 256 KiB-aligned base, returned to OS on free
 free(p): hdr = p & ~(256K-1)  → size class lives in the span header → no per-object header
```

## Arenas
Everything below the thread cache is sharded into **arenas** (default one per CPU, max 64; override with `QA_ARENAS=n`). An arena owns its own span supply (page heap over 4 MiB mmap chunks) and its own central free list per size class, each behind its own lock, so threads bound to different arenas never contend.
- A thread is bound to an arena round-robin when it first gets a `ThreadCache` (rebound when a recycled cache is reused).
- Every span header stores its **owner arena**. When a thread cache overflows, `bin_release` partitions the batch by owner and returns each group to the owning arena's central list — one lock in the common (all-local) case, one per distinct owner otherwise. So memory freed remotely goes home instead of migrating to the freeing thread's arena (tested by a ping-pong A-allocs/B-frees test whose span count stays flat).
- Trade-off: each arena keeps its own partially carved span per active size class, so virtual footprint scales with arenas × active classes (same as ptmalloc). `QA_ARENAS` lets you cap it.

## Design decisions
- **Lock-free fast path.** Alloc/free are a TLS load + pointer pop/push. No atomics, no locks.
- **Batching.** Thread caches exchange objects with the central list in batches sized by class (`32 KiB / size`, clamped to 2..64) and release one batch at `2×batch` for hysteresis, so lock traffic is amortised.
- **Zero per-object metadata.** Spans are 256 KiB-aligned; the header at the span base records the size class, so `free` is a mask + load. 16-byte alignment for free; `memalign` up to 128 KiB handled by picking a class whose size is a multiple of the alignment (small) or by offsetting inside an aligned mapping (large).
- **Remote frees are safe by construction.** An object freed by thread B just goes into B's bin for that class; overflow flows back to the shared central list.
- **Thread lifecycle.** `pthread_key` destructor flushes a dying thread's cache to central and recycles the `ThreadCache` struct, so thread churn doesn't leak (tested with 2000 short-lived threads).
- **Bootstrap-safe.** All globals are constant-initialised (no static constructors), metadata comes from private mmap, and the TLS pointer is published *before* `pthread_setspecific` (which can itself call `calloc`), so it works as a preloaded allocator under real programs (`ls`, `python3`, a full `g++` compile).
- `calloc` skips `memset` for large blocks (fresh `mmap` pages are already zero) and does overflow-checked multiplication.

## Build / test / bench
```
make            # libqalloc.so, test_qalloc, bench
make test       # unit + stress tests
bash scripts/check.sh   # TSan run + LD_PRELOAD smoke tests (use `setarch x86_64 -R` on WSL2 for TSan)
make bench      # same binary against glibc vs LD_PRELOAD=libqalloc.so
LD_PRELOAD=$PWD/build/libqalloc.so ./your_program
```

## Results (WSL2 Ubuntu 22.04, g++ 11 -O2, 16 logical cores; ns per malloc+free, lower is better; multi-thread rows are wall time ÷ total ops)

| benchmark                      | glibc 2.35 | qalloc |
|--------------------------------|-----------:|-------:|
| fixed 16 B alloc+free, 1T      | 12.4 | **6.1** |
| fixed 256 B alloc+free, 1T     | 12.9 | **6.1** |
| random 16 B–1 KiB churn, 1T    | 68.2 | **38.1** |
| random 16 B–1 KiB churn, 8T    | 11.2 | **7.3** |
| random 16 B–16 KiB churn, 4T   | 74.6 | **24.9** |
| producer→consumer, 2 pairs (all frees remote) | 216.0 | **46.1** |
| producer→consumer, 4 pairs     | 117.3 | **31.0** |

Numbers are from a noisy laptop VM, single run; use them for the shape, not as a rigorous claim. Run on bare metal with `taskset`/`perf stat` before quoting externally.

## Testing
Pattern-fill/verify under random alloc/free (no overlap/corruption), alignment sweep 16 B–64 KiB, `realloc` across class and small↔large boundaries, `calloc` zeroing over dirtied memory, multi-thread random stress, cross-thread producer/consumer frees, thread churn, and footprint checks (steady-state alloc/free doesn't grow the heap; large blocks return to the OS). Clean under ThreadSanitizer.

## Known limitations (and what I'd do next)
- Small-object memory is never returned to the OS (spans are recycled, not unmapped). Next: per-span use counts + `madvise(MADV_DONTNEED)` / page-heap coalescing.
- 32 KiB–256 KiB blocks each cost an `mmap`/`munmap`. Next: a medium-size page heap with best-fit free spans and a large-block cache.
- Arenas are assigned round-robin, not by CPU or load; next: per-CPU caches (rseq) or contention-aware rebinding. I have not benchmarked arena count vs. contention rigorously (thread caches absorb most traffic on my workloads), so no speed-up is claimed for arenas specifically.
- No `fork` handlers, no hardening (double-free detection, guard pages, junk-fill) — would add as a debug build.
- Linux only.

## CV bullets (pick 2–3)
- Built a tcmalloc-style general-purpose allocator in C++17 (thread-local caches → per-arena batched central free lists → per-arena 256 KiB aligned-span page heaps, with owner-arena routing of remote frees; direct-mmap large blocks), a header-free `free` path via address masking, and a process-wide `LD_PRELOAD` drop-in for the full malloc family (`memalign`, `posix_memalign`, `realloc`, `malloc_usable_size`).
- Achieved ~2× lower latency than glibc malloc on the single-thread fast path (6 ns vs 12 ns per alloc/free pair) and 3–5× on cross-thread-free workloads; validated against ThreadSanitizer, multi-thread stress/footprint tests, and real programs (`python3`, `g++`) under `LD_PRELOAD`.
