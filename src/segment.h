/*
 * hmalloc — segments, pages, and O(1) pointer -> metadata recovery (M3).
 *
 * Memory is organized in two layers:
 *
 *   Segment  — a SEGMENT_SIZE (4 MiB), segment-aligned region from the OS layer.
 *              Page index 0 holds this header (the Segment struct, including the
 *              per-page metadata array); pages 1..PAGES_PER_SEGMENT-1 are the
 *              data areas handed to size classes.
 *   Page     — one PAGE_SIZE (64 KiB) slice of a segment, serving exactly one
 *              size class. Its metadata (free lists, owner, counts) lives in the
 *              segment header, NOT in the page's data area.
 *
 * Because segments are segment-aligned, any pointer into a small allocation
 * recovers its owning Segment, Page, size class, and Heap with a couple of
 * shifts and a mask — no global lookup table. See DESIGN.md §4.
 *
 * Large allocations (> SMALL_SIZE_MAX) live in their own segment-aligned region
 * fronted by a tiny LargeHeader. A single tag byte at the region base (`kind`,
 * the first member of both Segment and LargeHeader) lets free() tell them apart.
 */
#ifndef HMALLOC_SEGMENT_H
#define HMALLOC_SEGMENT_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "constants.h"

namespace hm {

struct Heap;  // per-thread heap, defined in heap.h

// A free block, threaded through the freed object's own memory. Every block is
// >= ALLOC_ALIGN bytes, so there is always room for this pointer.
struct Block {
  Block *next;
};

enum class SegmentKind : std::uint8_t { Small = 0, Large = 1 };

// Hardware cache line; we keep owner-private page fields and the remotely-written
// thread_free list on separate lines to avoid false sharing on producer/consumer
// workloads (one thread allocates, another frees).
inline constexpr std::size_t CACHE_LINE = 64;

// Per-page metadata. Sized to two cache lines: line 0 is mutated only by the
// owning thread (no synchronization); line 1 holds the atomic cross-thread free
// list that other threads push onto.
struct alignas(CACHE_LINE) Page {
  // --- owner-private (cache line 0) ---------------------------------------
  Block *free;          // local free list; pop on alloc, push on local free
  std::uint32_t used;   // blocks handed out (owner's view; see heap.cpp)
  std::uint32_t capacity;     // total blocks this page can ever hold
  std::uint32_t reserved;     // blocks carved from the bump area so far
  std::uint32_t block_size;   // bytes per block (0 if page is unused)
  std::uint32_t size_class;   // class index this page serves
  bool in_use;                // assigned to a class (vs. free in central heap)
  // Owning thread heap. Atomic because a remote freer reads it on the lock-free
  // path while thread-exit may be clearing it (abandonment). Any observed value
  // other than the reader's own heap routes to the remote path, which is always
  // correct, so the hot read can be relaxed.
  std::atomic<Heap *> owner;
  Page *next;                 // links: heap per-class list, or central free list
  Page *prev;                 // (prev only used by the heap's doubly-linked list)
  std::uint8_t *area;         // base of this page's data region

  // --- remotely written (cache line 1) ------------------------------------
  alignas(CACHE_LINE) std::atomic<Block *> thread_free;
};

// Number of pages in a segment usable for allocations (page 0 is the header).
inline constexpr std::size_t USABLE_PAGES_PER_SEGMENT =
    PAGES_PER_SEGMENT - FIRST_USABLE_PAGE;

// Segment header, living at the base of a Small segment (page index 0). All
// fields after `kind` are owned by the central heap under its lock.
struct alignas(CACHE_LINE) Segment {
  SegmentKind kind;            // MUST be first: shared tag with LargeHeader
  std::uint32_t page_count;    // PAGES_PER_SEGMENT
  std::size_t mmap_size;       // bytes mapped (== SEGMENT_SIZE for small)
  Segment *next;               // central heap's list of all small segments
  // Per-segment pool of currently-free pages (linked via Page::next). Keeping
  // the pool per segment makes "is this segment fully free?" O(1) and lets a
  // fully-free segment be unmapped without scanning a global free list.
  Page *free_list;
  std::uint32_t free_count;    // pages in free_list
  // Membership in the central "segments with >= 1 free page" doubly-linked list.
  Segment *avail_prev;
  Segment *avail_next;
  bool in_avail;
  Page pages[PAGES_PER_SEGMENT];
};

// Header for a large allocation's region. `kind` aliases Segment::kind so the
// tag byte at the region base identifies the kind for any pointer.
struct LargeHeader {
  SegmentKind kind;       // MUST be first
  std::size_t mmap_size;  // total bytes mapped for the region
  std::size_t offset;     // bytes from region base to the user pointer
  std::size_t req_size;   // size the user actually requested
};

// Bytes from a large region's base to the (default-aligned) user pointer.
inline constexpr std::size_t LARGE_OFFSET =
    ((sizeof(LargeHeader) + (ALLOC_ALIGN - 1)) & ~(ALLOC_ALIGN - 1));

// --- O(1) recovery from a pointer ------------------------------------------

// Region base (Segment or LargeHeader) for any pointer within it.
inline void *region_base(const void *p) {
  return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(p) &
                                  ~SEGMENT_MASK);
}

// The kind tag at a region base. Valid only for pointers hmalloc returned.
inline SegmentKind kind_of(const void *p) {
  return *static_cast<const SegmentKind *>(region_base(p));
}

inline Segment *segment_of(const void *p) {
  return static_cast<Segment *>(region_base(p));
}

// Page index (0..PAGES_PER_SEGMENT-1) for a pointer within a small segment.
inline std::size_t page_index_of(const void *p) {
  return (reinterpret_cast<std::uintptr_t>(p) & SEGMENT_MASK) >> PAGE_SHIFT;
}

// Owning Page for a small-allocation pointer.
inline Page *page_of(Segment *s, const void *p) {
  return &s->pages[page_index_of(p)];
}

}  // namespace hm

#endif  // HMALLOC_SEGMENT_H
