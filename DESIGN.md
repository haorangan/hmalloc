# hmalloc — Design

This document describes the architecture of `hmalloc` and the reasoning behind
each decision. It doubles as the implementation spec; sections are marked with
the milestone (M1–M8) that lands them.

> **Status:** all milestones (M1–M8) are implemented and live; the code maps to
> the files in [`src/`](src/) (`os`, `size_class`, `segment`, `central`, `heap`,
> `region_registry`, `hmalloc`). Two simplifications relative to the most
> aggressive designs: abandoned pages from exited threads are reclaimed by a
> central sweep once fully free (rather than adopted into another live heap), and
> `free` recovers the block start by mask/shift only (no reciprocal-multiply
> needed, because aligned allocations are placed on true block boundaries — §9).

## 1. Problem framing

A general-purpose allocator must serve allocations ranging from a few bytes to
many megabytes, from many threads, with three competing objectives:

1. **Speed** — the common path (`malloc` of a small object, `free` of an object
   the current thread allocated) must be nearly free.
2. **Scalability** — performance must not collapse when many threads allocate
   concurrently, or when objects allocated on one thread are freed on another
   (the *producer/consumer* pattern, common in queues and pipelines).
3. **Low fragmentation** — memory handed back to the program but not in use
   should stay bounded relative to live data.

The classic tension: a single global heap with one lock is simple and
low-fragmentation but does not scale; pure per-thread heaps scale but leak memory
under cross-thread frees. Modern allocators resolve this with **per-thread heaps
plus free-list sharding**, which `hmalloc` adopts.

## 2. Size classes (M2)

Allocations are rounded up to one of a fixed set of **size classes**. Small
objects use a geometric-ish spacing (powers of two subdivided into 4 steps, e.g.
`16, 32, 48, 64, 80, 96, 112, 128, 160, 192, ...`) to bound worst-case internal
fragmentation to ~25% while keeping the class count small (~40–50 classes up to
the small/large threshold).

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
the segment header is found by masking off the low bits — no global table.

A segment is subdivided into fixed-size **pages** (e.g. 64 KiB). Each page is
assigned to a single size class on demand and carries a small header describing
its class, free list, and owning heap.

Large allocations are individual `mmap` regions tracked separately and returned
to the OS on free (with an optional size-bucketed cache to avoid syscall churn).

## 4. Pointer → metadata in O(1) (M3)

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
    if (p->free_list):                  // fast path: no lock, no syscall
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

The defining trick of mimalloc. Each page has **two** free lists:

- `local_free` — pushed/popped only by the owning thread; needs no
  synchronization.
- `thread_free` — an atomic, lock-free stack (`compare_exchange` on the head)
  that *other* threads push onto when they free an object belonging to this page.

The owner periodically (when its `local_free` empties) **collects** `thread_free`
in one atomic swap and splices it into `local_free`. Consequences:

- The owner's fast path never touches an atomic.
- Remote frees are a single CAS, contending only with other remote-freers of the
  *same page*, not a global lock.
- Memory ordering: the remote push uses `release`, the owner's collect uses
  `acquire`, establishing happens-before on the freed object's bytes.

## 7. Central heap + recycling (M6)

A **central heap** owns segments and free pages not currently held by any thread.
When a thread heap needs a page for a class and has none, it requests one from the
central heap; when a page becomes fully free, it is returned for reuse by any
class. Segments with all pages free can be `munmap`'d or retained in a small cache
to balance RSS against syscall cost. The central heap is the only component that
takes a lock, and only off the fast path.

## 8. Fragmentation strategy

- **Internal:** bounded by size-class spacing (§2), measured by comparing
  requested vs. served bytes.
- **External:** one class per page means freed slots are always reusable by the
  same class; full-empty page recycling (§7) returns space across classes.
- Benchmarks (M7) report RSS over time under churn workloads, not just peak.

## 9. API surface (M8)

`hm_malloc`, `hm_free`, `hm_calloc`, `hm_realloc`, `hm_aligned_alloc`,
`hm_usable_size`, plus `hm_stats()` for live counters (bytes in use, segments
mapped, fast-path hit rate). An optional `-DHMALLOC_OVERRIDE` mode aliases the
standard names for drop-in `LD_PRELOAD`/`DYLD_INSERT_LIBRARIES` testing.

## 10. Benchmarks (M7)

Against system malloc, jemalloc, and mimalloc:

- **Single-thread throughput** — `malloc`/`free` of fixed and mixed sizes.
- **Multi-thread scaling** — N threads each churning independently (1→cores).
- **Producer/consumer** — thread A allocates, thread B frees (stresses §6).
- **Fragmentation** — RSS under long-running mixed-size churn.

Each benchmark reports ns/op, ops/s, and (where relevant) peak/steady RSS, with
methodology documented so results are reproducible.
