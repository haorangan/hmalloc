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

> **Status:** active development. See the [roadmap](#roadmap) for what works today.
> The current build links a thin libc-backed stub so the test and benchmark
> harnesses run end-to-end while the real internals land milestone by milestone.

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
./build/bench                                  # run benchmarks
```

Requires a C++20 compiler. Developed against Apple Clang / LLVM on arm64 and
x86-64 Linux.

## Roadmap

- [ ] **M0** — Scaffold, build system, test + benchmark harness *(in progress)*
- [ ] **M1** — OS memory layer (`mmap` arenas) + large-object passthrough
- [ ] **M2** — Size classes + single-threaded free-list allocator
- [ ] **M3** — Segment/page metadata + O(1) pointer → page lookup
- [ ] **M4** — Per-thread heaps + lock-free fast path
- [ ] **M5** — Cross-thread frees (atomic thread-free list) + reclaim
- [ ] **M6** — Central heap + page/segment recycling
- [ ] **M7** — Benchmarks vs system malloc / jemalloc / mimalloc
- [ ] **M8** — Full API surface, runtime stats, design writeup

## License

MIT — see [LICENSE](LICENSE).
