# hmalloc

hmalloc is a `malloc`/`free` replacement for C and C++, written from scratch. It
borrows its structure from [mimalloc](https://github.com/microsoft/mimalloc) and
[tcmalloc](https://github.com/google/tcmalloc): requests are rounded to a set of
size classes, each thread gets its own heap so the common path takes no locks,
and a free that comes from a different thread is parked on a separate per-page
list so it never gets in the way of the thread that owns the memory.

It isn't trying to beat those allocators. The point was to build the parts that
make a modern allocator fast, understand why each one matters, and back the
claims with benchmarks you can run yourself.

The allocator is finished — all the milestones at the bottom are done. Small
objects come from per-thread heaps carved out of 4 MiB `mmap`'d segments; anything
over 16 KiB gets its own region. Cross-thread frees use a lock-free per-page list.
The whole test suite runs clean under ThreadSanitizer and UBSan.

## How it works

```
malloc(sz) ──► size class ──► thread-local free list (pop)        ◄─ fast path, no lock
                                   │ empty?
                                   ▼
                              thread heap ──► page (span of one size class)
                                   │ none with free slots?
                                   ▼
                            central heap  ──► segment (mmap'd, aligned region)
```

Most calls never get past the first line: pop a block off the current thread's
free list for that size class and return it. No lock, no atomic, no syscall.

What makes `free` just as cheap is that segments are aligned to their own size.
Given any pointer, masking off the low bits lands on the segment header, and a
shift picks out the page inside it — and the page already records its size class
and which thread owns it. So `free` finds everything it needs with a couple of
instructions and no global lookup table. The reasoning behind the rest of the
design is in [DESIGN.md](DESIGN.md).

## Building

You need a C++20 compiler. It's been built with Apple Clang on arm64 and with
LLVM/GCC on x86-64 Linux.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # tests
./build/bench                                  # all benchmarks
./build/bench scaling                          # or just one
```

The concurrency code is the easiest part to get subtly wrong, so the threaded
tests are also run under sanitizers in CI-style checks:

```sh
cmake -S . -B build-tsan -DHM_SANITIZE=thread && cmake --build build-tsan
./build-tsan/test_threads
```

`HM_SANITIZE` also takes `address` or `undefined`.

## Using it as your allocator

Build the override library and preload it; the standard C entry points and the
C++ `operator new`/`delete` family all route through hmalloc with no recompile:

```sh
cmake -S . -B build -DHMALLOC_OVERRIDE=ON && cmake --build build

LD_PRELOAD=./build/libhmalloc_override.so ./your_program          # Linux
DYLD_INSERT_LIBRARIES=./build/libhmalloc_override.dylib \         # macOS
  DYLD_FORCE_FLAT_NAMESPACE=1 ./your_program
```

A program that's fully interposed will hand back the odd pointer that some other
allocator produced before the override was live (allocations made during process
startup, mostly). Those get forwarded to the real `free` instead of being treated
as ours. The check looks the pointer up in a registry of regions hmalloc mapped,
so it decides from the address alone and never dereferences a pointer it didn't
hand out.

If you call the API directly, `hm_stats()` returns the live picture — bytes
mapped, segments, pages in use, large allocations. Building with `-DHMALLOC_STATS`
adds malloc/fast-path/slow-path counts; they're off by default so the fast path
carries no counters.

## Benchmarks

Numbers below are from an Apple M-class machine (arm64, 10 cores, Release build),
where one "op" is a `malloc` plus a `free` with a write in between. They compare
against the macOS system allocator. Treat them as a rough picture of where the
design helps, not a promise about your workload — and if you want a fairer fight,
preload jemalloc or mimalloc and run the same `./build/bench`.

| Scenario | system | hmalloc |
|---|---:|---:|
| single thread, 64 B fixed | 37 Mops/s | 139 Mops/s |
| single thread, mixed sizes | 26 Mops/s | 124 Mops/s |
| producer/consumer (free on another thread) | 4.5 Mops/s | 9.0 Mops/s |
| 10 threads, each churning independently | 27 Mops/s | 653 Mops/s |

The single-thread gap comes from how little the fast path does (about 7 ns per
op). The more interesting result is the last row: because each thread allocates
out of its own heap, throughput climbs with core count instead of flattening, and
the system allocator actually slows down as threads contend for it.

The catch shows up in the fragmentation benchmark, where hmalloc holds a larger
resident set for the same set of live objects (around 616 MiB versus 484 MiB).
That's the price of size classes — rounding every request up to a class wastes up
to ~25% per object, and pages are handed out whole. The speed is paid for partly
in memory. `./build/bench frag` prints it.

## What's done

- [x] **M0** — scaffold, build system, test and benchmark harness
- [x] **M1** — OS layer: `mmap`'d aligned segments, large-object passthrough
- [x] **M2** — size classes and the free-list allocator
- [x] **M3** — segment/page metadata and O(1) pointer → page lookup
- [x] **M4** — per-thread heaps and the lock-free fast path
- [x] **M5** — cross-thread frees (atomic per-page list) and collection
- [x] **M6** — central heap, page/segment recycling, thread-exit reclaim
- [x] **M7** — benchmarks (against system malloc; preload others to compare)
- [x] **M8** — full API, `hm_stats`, drop-in override

A few things I'd do next: when a thread exits with pages that still hold live
objects, adopt those pages into another running heap instead of waiting for the
central sweep to reclaim them once they empty; replace the division in the
interior-pointer `free` path with a precomputed reciprocal multiply; and return
memory to the OS more eagerly by purging idle pages.

## License

MIT — see [LICENSE](LICENSE).
