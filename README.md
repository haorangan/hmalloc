# hmalloc

A modern, high-performance memory allocator for C/C++ — built from scratch.

`hmalloc` is a general-purpose `malloc` replacement in the lineage of
[mimalloc](https://github.com/microsoft/mimalloc) and
[tcmalloc](https://github.com/google/tcmalloc): segregated **size classes**,
per-thread **heaps** for a lock-free common path, and **free-list sharding** so
that frees from another thread never contend with the owner's fast path.

The goal is not to beat the state of the art — it is to implement the core ideas
that make a modern allocator fast, and to *measure and explain* every design
decision against the system allocator and jemalloc/mimalloc.

> **Status:** the full allocator is implemented (milestones M0–M8). The libc
> stub is gone: small allocations go through per-thread heaps over size-classed
> pages in mmap'd segments, large allocations get their own segment-aligned
> regions, and cross-thread frees use a lock-free per-page list. The test suite
> is clean under ThreadSanitizer and UBSan. See [Results](#results) for numbers.

## Design goals

- **Fast common path.** Allocation and same-thread free should be a handful of
  instructions with no locks and no syscalls.
- **Scalable.** Throughput should grow with cores, not collapse under
  cross-thread free contention.
- **Low fragmentation.** Segregated size classes + page reuse keep internal and
  external fragmentation bounded and measurable.
- **Honest.** Every claim in [DESIGN.md](DESIGN.md) is backed by a benchmark in
  [`bench/`](bench/).

## Architecture at a glance

```
malloc(sz) ──► size class ──► thread-local free list (pop)        ◄─ fast path, no lock
                                   │ empty?
                                   ▼
                              thread heap ──► page (span of one size class)
                                   │ none with free slots?
                                   ▼
                            central heap  ──► segment (mmap'd, aligned region)
```

Pointer → metadata is **O(1)**: segments are aligned to a fixed size, so the
owning page (and thus the size class and heap) is recovered from a freed pointer
by masking, with no global lookup table. Full details in
[DESIGN.md](DESIGN.md).

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # run tests
./build/bench                                  # run all benchmarks
./build/bench scaling                          # or one scenario
```

Requires a C++20 compiler. Developed against Apple Clang / LLVM on arm64 and
x86-64 Linux.

**Sanitizers.** The concurrency paths are validated under TSan and UBSan:

```sh
cmake -S . -B build-tsan -DHM_SANITIZE=thread && cmake --build build-tsan
./build-tsan/test_threads
```

**Drop-in replacement.** Build the override shim and preload it into any program:

```sh
cmake -S . -B build -DHMALLOC_OVERRIDE=ON && cmake --build build
# Linux:
LD_PRELOAD=./build/libhmalloc_override.so ./your_program
# macOS:
DYLD_INSERT_LIBRARIES=./build/libhmalloc_override.dylib \
  DYLD_FORCE_FLAT_NAMESPACE=1 ./your_program
```

The shim routes the standard C entry points and C++ `operator new`/`delete` to
hmalloc, and forwards pointers it didn't allocate (e.g. from process startup) to
the real allocator via a fault-safe ownership check.

**Stats.** `hm_stats()` reports live memory, segments, pages, and large
allocations. Build with `-DHMALLOC_STATS` to also track malloc/fast-path/slow-path
counts (off by default so the hot path stays counter-free).

## Results

Apple M-class, arm64, 10 hardware threads, Release. One op = one `malloc` + one
`free`, touched. These are favorable in-process microbenchmarks against the macOS
system allocator; treat them as directional, not a substitute for measuring your
own workload.

| Scenario | system | hmalloc | speedup |
|---|---:|---:|---:|
| single-thread, 64 B fixed | 37 Mops/s | 139 Mops/s | 3.7x |
| single-thread, mixed sizes | 26 Mops/s | 124 Mops/s | 4.8x |
| producer/consumer (cross-thread) | 4.5 Mops/s | 9.0 Mops/s | 2.0x |
| 10-thread scaling | 27 Mops/s | 653 Mops/s | 24x |

The fast path is a lock-free free-list pop (~7 ns/op). hmalloc **scales with
cores** (1→10 threads: 132→653 Mops/s, 4.9x) where the system allocator
*regresses* under contention.

**The tradeoff:** on a long-running mixed-size churn benchmark hmalloc holds a
higher resident set than the system allocator (~616 vs ~484 MiB for the same live
set). That is the cost of segregated size classes — internal fragmentation is
bounded at ~25% by class spacing, plus per-page granularity. Throughput and
scalability are bought with some memory. `bench frag` reports it; don't hide it.

## Roadmap

- [x] **M0** — Scaffold, build system, test + benchmark harness
- [x] **M1** — OS memory layer (`mmap` segments) + large-object passthrough
- [x] **M2** — Size classes + free-list allocator
- [x] **M3** — Segment/page metadata + O(1) pointer → page lookup
- [x] **M4** — Per-thread heaps + lock-free fast path
- [x] **M5** — Cross-thread frees (atomic thread-free list) + collect
- [x] **M6** — Central heap + page/segment recycling + thread-exit reclaim
- [x] **M7** — Benchmarks (system malloc; preload others to compare)
- [x] **M8** — Full API surface, `hm_stats`, drop-in override mode

Possible next steps: adopting abandoned pages into a live heap for a class (today
they are reclaimed by a central sweep once fully free), a reciprocal-multiply fast
path for `free` on interior pointers, and tighter RSS via page purging.

## License

MIT — see [LICENSE](LICENSE).
