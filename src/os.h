/*
 * hmalloc — OS memory layer (M1).
 *
 * The only component that talks to the kernel. Everything above it (segments,
 * pages, heaps) is built on aligned regions handed out here. Two responsibilities:
 *
 *   1. Hand out large, *power-of-two aligned* regions. Segment alignment is the
 *      structural keystone of hmalloc: given any pointer into a segment, the
 *      segment header is recovered by masking off the low bits (see segment.h),
 *      so we never need a global lookup table. mmap does not guarantee alignment,
 *      so os_alloc_aligned over-maps and trims the slop (see os.cpp).
 *
 *   2. Account every byte mapped/unmapped so tests can assert the allocator
 *      returns all memory to the OS (leak detection) and hm_stats() can report
 *      real RSS-relevant numbers.
 */
#ifndef HMALLOC_OS_H
#define HMALLOC_OS_H

#include <cstddef>
#include <cstdint>

namespace hm {

// The OS page size (4 KiB or 16 KiB depending on platform), queried once and
// cached. All sizes passed to os_alloc_aligned/os_free must be multiples of it.
std::size_t os_page_size();

// Map `size` bytes aligned to `alignment`. Both must be non-zero multiples of
// os_page_size(); `alignment` must additionally be a power of two. The returned
// region is zero-filled by the kernel. Returns nullptr on failure.
void *os_alloc_aligned(std::size_t size, std::size_t alignment);

// Unmap a region previously returned by os_alloc_aligned. `size` must be the
// exact size requested for that region.
void os_free(void *ptr, std::size_t size);

// Hint to the kernel that [ptr, ptr+size) is no longer needed, dropping its
// physical pages (RSS) while keeping the address range mapped and reusable.
// Used by the segment cache to shed memory without syscall churn on remap.
// Returns true on success. `ptr`/`size` should be OS-page aligned.
bool os_purge(void *ptr, std::size_t size);

// Live accounting, for hm_stats() and for leak assertions in tests.
struct OsStats {
  std::size_t bytes_mapped;   // currently mapped (alloc - free), in bytes
  std::size_t peak_mapped;    // high-water mark of bytes_mapped
  std::uint64_t map_calls;    // total os_alloc_aligned successes
  std::uint64_t unmap_calls;  // total os_free calls
};
OsStats os_stats();

}  // namespace hm

#endif  // HMALLOC_OS_H
