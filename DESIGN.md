# hmalloc Design

This document describes the architecture of `hmalloc` and the reasoning behind
each decision. It also serves as the implementation spec, and each section is
marked with the milestone, from M1 to M8, that lands it.

Everything described here is implemented. The sections map onto the files in
[`src/`](src/): the OS layer is `os`, size classes are `size_class`, the segment
and page structures are `segment`, the per-thread heap is `heap`, the shared pool
is `central`, and the public API is `hmalloc`.

Two places where the implementation takes a simpler road than the most aggressive
allocators are worth calling out up front. When a thread exits holding pages that
still have live objects, those pages are reclaimed by a sweep in the central heap
once their last object is freed, rather than being adopted into another running
thread's heap. And `free` finds the start of a block with a mask and a shift
instead of the reciprocal-multiply trick mimalloc uses, because aligned
allocations are already placed on real block boundaries (see §9), so there is no
interior pointer to round down.

## 1. Problem framing

A general-purpose allocator must serve allocations that range from a few bytes to
many megabytes, coming from many threads, while it balances three competing
objectives: speed, scalability, and low fragmentation.

1. Speed means that the common path, which is a `malloc` of a small object or a
   `free` of an object the current thread allocated, has to be nearly free.
2. Scalability means that performance does not collapse when many threads allocate
   at the same time, or when objects allocated on one thread are freed on another.
   That second case is the producer and consumer pattern, and it is common in
   queues and pipelines.
3. Low fragmentation means that memory which has been handed back to the program
   but is not currently in use stays bounded relative to the live data.

There is a classic tension here. A single global heap protected by one lock is
simple and keeps fragmentation low, but it does not scale. Pure per-thread heaps
scale well, but they leak memory under cross-thread frees. Modern allocators
resolve this by combining per-thread heaps with free-list sharding, and that is
the approach hmalloc takes.

## 2. Size classes (M2)

Allocations are rounded up to one of a fixed set of **size classes**. Small
objects use a geometric-ish spacing (powers of two subdivided into 4 steps, e.g.
`16, 32, 48, 64, 80, 96, 112, 128, 160, 192, ...`) to bound worst-case internal
fragmentation to about 25% while keeping the class count small, at roughly 40 to
50 classes up to the threshold between small and large objects.

- `size_class(n)` maps a request to a class index in O(1) via a small lookup
  table for the dense low range and bit tricks above it.
- Each class has a fixed object size; a page only ever serves one class, so every
  free slot in a page is interchangeable.

**Large objects** (above the small threshold, e.g. > 16 KiB) skip the size-class
machinery and are served directly from the OS layer (§3), page-rounded.

## 3. OS memory layer (M1)

All memory comes from the OS in large, aligned chunks called **segments**
(e.g. 4 MiB), obtained via `mmap(MAP_ANON)` (`VirtualAlloc` on Windows, later).
Segments are aligned to their own size so that, given any pointer into a segment,
the segment header is found by masking off the low bits, with no global table
involved.

A segment is subdivided into fixed-size **pages** (e.g. 64 KiB). Each page is
assigned to a single size class on demand and carries a small header describing
its class, free list, and owning heap.

Large allocations are individual `mmap` regions tracked separately and returned
to the OS on free (with an optional size-bucketed cache to avoid syscall churn).

## 4. Pointer to metadata in O(1) (M3)

`free(ptr)` must recover the object's size class and owning page with no lookup:

```
segment = ptr & ~(SEGMENT_SIZE - 1)     // segment is segment-aligned
page    = segment->page_for(ptr)        // (ptr - segment_base) / PAGE_SIZE
class   = page->size_class
```

This is the single most important structural decision: it makes `free` O(1) and
branch-light, and it is why segments must be power-of-two aligned.

## 5. Per-thread heaps + fast path (M4)

Each thread owns a **heap**: an array of pages indexed by size class, each with a
**local free list** of available slots. The hot paths become:

```
void* malloc(sz):
    c = size_class(sz)
    p = heap.pages[c]
    if (p->free_list):                  // fast path that needs no lock or syscall
        return pop(p->free_list)
    return malloc_slow(c)               // refill from heap / central / OS

void free(ptr):
    page = page_of(ptr)                 // §4
    if (page->owner == this_thread):    // fast path
        push(page->local_free, ptr)
    else:
        free_remote(page, ptr)          // §6
```

The thread heap is a `thread_local` pointer, initialized lazily on first use and
detached on thread exit (its pages are handed back to the central heap, §7).

## 6. Cross-thread frees: free-list sharding (M5)

This is the defining trick of mimalloc. Each page has two free lists. The first,
`local_free`, is pushed and popped only by the owning thread, so it needs no
synchronization. The second, `thread_free`, is an atomic, lock-free stack that
uses `compare_exchange` on the head, and other threads push onto it when they free
an object that belongs to this page.

When its `local_free` empties, the owner collects `thread_free` in one atomic swap
and splices it into `local_free`. This arrangement has three consequences. The
owner's fast path never touches an atomic. A remote free is a single
compare-and-swap that contends only with other remote frees of the same page
rather than with a global lock. The ordering is correct because the remote push
uses `release` and the owner's collect uses `acquire`, which establishes a
happens-before relationship on the freed object's bytes.

## 7. Central heap + recycling (M6)

A **central heap** owns segments and free pages not currently held by any thread.
When a thread heap needs a page for a class and has none, it requests one from the
central heap; when a page becomes fully free, it is returned for reuse by any
class. Segments with all pages free can be `munmap`'d or retained in a small cache
to balance RSS against syscall cost. The central heap is the only component that
takes a lock, and only off the fast path.

## 8. Fragmentation strategy

Internal fragmentation is bounded by the size-class spacing from §2, and it is
measured by comparing requested bytes against served bytes. External fragmentation
is limited because one class per page keeps freed slots reusable by that same
class, and the recycling of full and empty pages from §7 returns space across
classes. The benchmarks in M7 report resident set size over time under churn
workloads rather than only the peak.

## 9. API surface (M8)

`hm_malloc`, `hm_free`, `hm_calloc`, `hm_realloc`, `hm_aligned_alloc`,
`hm_usable_size`, plus `hm_stats()` for live counters (bytes in use, segments
mapped, fast-path hit rate). An optional `-DHMALLOC_OVERRIDE` mode aliases the
standard names for drop-in `LD_PRELOAD`/`DYLD_INSERT_LIBRARIES` testing.

## 10. Benchmarks (M7)

The suite measures against system malloc, and you can preload jemalloc or
mimalloc to compare against them as well. It covers four workloads. Single-thread
throughput runs `malloc` and `free` over fixed and mixed sizes. Multi-thread
scaling runs N threads that each churn independently, from one thread up to the
core count. The producer and consumer workload has one thread allocate while
another frees, which stresses the path described in §6. The fragmentation workload
measures resident set size under long-running mixed-size churn.

Each benchmark reports nanoseconds per operation, operations per second, and,
where it is relevant, the peak and steady resident set size. The methodology is
documented so that the results are reproducible.
