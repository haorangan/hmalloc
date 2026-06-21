# hmalloc

hmalloc is a `malloc` and `free` replacement for C and C++, written from scratch.
It borrows its structure from [mimalloc](https://github.com/microsoft/mimalloc)
and [tcmalloc](https://github.com/google/tcmalloc). Requests are rounded to a set
of size classes, each thread gets its own heap so that the common path takes no
locks, and a free that arrives from a different thread is parked on a separate
per-page list so that it never slows down the thread that owns the memory.

It is not trying to beat those allocators. The point was to build the parts that
make a modern allocator fast, to understand why each one matters, and to back the
claims with benchmarks that you can run yourself.

The allocator is finished, and every milestone listed at the bottom is done.
Small objects come from per-thread heaps that are carved out of 4 MiB segments
obtained with `mmap`, while any request larger than 16 KiB gets its own region.
Frees that cross threads use a lock-free per-page list. The whole test suite runs
clean under ThreadSanitizer and UBSan.

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

Most calls never get past the first line. The allocator pops a block off the
current thread's free list for that size class and returns it, and that path does
not take a lock, perform an atomic operation, or make a system call.

Freeing is cheap for a different reason, which is that segments are aligned to
their own size. Given any pointer, masking off the low bits lands on the segment
header, and a shift selects the page inside it. Because each page already records
its size class and the thread that owns it, `free` recovers everything it needs
with a couple of instructions and without consulting any global table. The rest of
the reasoning behind the design is written up in [DESIGN.md](DESIGN.md).

## Building

You need a C++20 compiler. The code has been built with Apple Clang on arm64 and
with LLVM and GCC on x86-64 Linux.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # tests
./build/bench                                  # all benchmarks
./build/bench scaling                          # or just one
```

The concurrency code is the easiest part to get subtly wrong, so the threaded
tests are also run under sanitizers.

```sh
cmake -S . -B build-tsan -DHM_SANITIZE=thread && cmake --build build-tsan
./build-tsan/test_threads
```

The `HM_SANITIZE` option also accepts `address` and `undefined`.

## Using it as your allocator

Build the override library and preload it. The standard C entry points and the
C++ `operator new` and `operator delete` family all route through hmalloc without
recompiling your program.

```sh
cmake -S . -B build -DHMALLOC_OVERRIDE=ON && cmake --build build

LD_PRELOAD=./build/libhmalloc_override.so ./your_program          # Linux
DYLD_INSERT_LIBRARIES=./build/libhmalloc_override.dylib \         # macOS
  DYLD_FORCE_FLAT_NAMESPACE=1 ./your_program
```

When a program is fully interposed it will occasionally hand back a pointer that
some other allocator produced before the override became active, which usually
means an allocation made during process startup. Those pointers are forwarded to
the real `free` instead of being treated as ours. The check looks the pointer up
in a registry of the regions that hmalloc has mapped, so it decides from the
address alone and never dereferences a pointer that it did not hand out.

If you call the API directly, `hm_stats()` returns the live picture, including
bytes mapped, segments, pages in use, and large allocations. Building with
`-DHMALLOC_STATS` adds counts of total allocations and of how often the fast and
slow paths were taken. Those counters are off by default so that the fast path
carries no extra work.

## Benchmarks

The numbers below come from an Apple M-class machine with 10 cores running a
Release build, where one operation is a `malloc` followed by a `free` with a write
in between. They are measured against the macOS system allocator. They should be
read as a rough picture of where the design helps rather than a promise about your
own workload, and if you want a fairer comparison you can preload jemalloc or
mimalloc and run the same `./build/bench`.

| Scenario | system | hmalloc |
|---|---:|---:|
| single thread, 64 B fixed | 37 Mops/s | 139 Mops/s |
| single thread, mixed sizes | 26 Mops/s | 124 Mops/s |
| producer/consumer (free on another thread) | 4.5 Mops/s | 9.0 Mops/s |
| 10 threads, each churning independently | 27 Mops/s | 653 Mops/s |

The single-thread gap comes from how little the fast path has to do, which works
out to roughly 7 nanoseconds per operation. The more interesting result is the
last row. Because every thread allocates out of its own heap, throughput climbs
with the core count instead of flattening, and the system allocator actually slows
down as its threads contend for shared state.

The cost shows up in the fragmentation benchmark, where hmalloc holds a larger
resident set than the system allocator for the same set of live objects, around
616 MiB against 484 MiB. This is the price of size classes. Rounding every request
up to a class wastes as much as a quarter of each object, and pages are handed out
whole, so the speed is paid for partly in memory. You can see the figure for
yourself by running `./build/bench frag`.

## What's done

Each milestone below is implemented and committed.

- M0 set up the scaffold, the build system, and the test and benchmark harness.
- M1 added the OS layer, which maps aligned segments and serves large objects
  directly.
- M2 added the size classes and the free-list allocator.
- M3 added the segment and page metadata and the constant-time recovery of a page
  from any pointer.
- M4 added the per-thread heaps and the lock-free fast path.
- M5 added cross-thread frees through an atomic per-page list, along with the
  collection step that folds them back in.
- M6 added the central heap, page and segment recycling, and reclamation of a
  thread's pages when it exits.
- M7 added the benchmark suite, which measures against the system allocator and
  lets you preload others to compare.
- M8 finished the public API, added `hm_stats`, and built the drop-in override.

There are a few things worth doing next. When a thread exits while it still holds
pages with live objects, those pages could be adopted into another running heap
instead of waiting for the central sweep to reclaim them once they empty. The
division in the interior-pointer path of `free` could be replaced with a
precomputed reciprocal multiply. Memory could be returned to the operating system
more eagerly by purging idle pages.

## License

This project is released under the MIT license. See the LICENSE file for details.
