/*
 * hmalloc — central heap (M3/M6).
 *
 * Owns all small segments and the pool of usable pages not currently held by
 * any thread heap. Thread heaps call in here only off the fast path: to acquire
 * a fresh page when they run out, or (M6) to return a page that became empty.
 * It is the one component that takes a lock, and never on the hot path.
 */
#ifndef HMALLOC_CENTRAL_H
#define HMALLOC_CENTRAL_H

#include <cstdint>

#include "segment.h"

namespace hm {

// Acquire a page for size class `cls` (block size `block_size`), owned by
// `owner`. Reuses a free page if one is available, otherwise maps a new
// segment. Returns nullptr on out-of-memory.
Page *central_acquire_page(Heap *owner, std::uint32_t cls,
                           std::uint32_t block_size);

// Return a now-empty page to the central pool for reuse by any size class.
void central_release_page(Page *pg);

// Hand a page that still has live objects to the central heap on thread exit.
// Its owner is cleared; remaining objects (freed later by other threads onto the
// page's thread_free list) are collected by a central sweep, and the page is
// reclaimed once it is fully free. The page must have been collected by its
// owner immediately before this call.
void central_abandon_page(Page *pg);

// Snapshot of central-heap occupancy, for hm_stats().
struct CentralStats {
  std::uint64_t segments;      // small segments currently mapped
  std::uint64_t pages_in_use;  // usable pages currently owned by a heap
  std::uint64_t free_pages;    // usable pages available for reuse
};
CentralStats central_stats();

}  // namespace hm

#endif  // HMALLOC_CENTRAL_H
