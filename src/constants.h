/*
 * hmalloc — core layout constants, shared by every internal module.
 *
 * These numbers define the geometry the whole allocator is built around. The
 * single most important property: SEGMENT_SIZE is a power of two and segments
 * are aligned to it, so for any pointer `p` into a segment,
 *
 *     segment_base = p & ~(SEGMENT_SIZE - 1)
 *
 * recovers the owning segment with one AND — no global table. Pages sit at
 * fixed offsets inside a segment, so the owning page (and thus size class and
 * heap) is recovered just as cheaply. See segment.h / DESIGN.md §4.
 */
#ifndef HMALLOC_CONSTANTS_H
#define HMALLOC_CONSTANTS_H

#include <cstddef>
#include <cstdint>

namespace hm {

// Segments: large, segment-aligned regions obtained from the OS layer.
inline constexpr std::size_t SEGMENT_SHIFT = 22;  // 4 MiB
inline constexpr std::size_t SEGMENT_SIZE = std::size_t(1) << SEGMENT_SHIFT;
inline constexpr std::size_t SEGMENT_MASK = SEGMENT_SIZE - 1;

// Pages: a segment is carved into fixed-size pages, each serving one size class.
inline constexpr std::size_t PAGE_SHIFT = 16;  // 64 KiB
inline constexpr std::size_t PAGE_SIZE = std::size_t(1) << PAGE_SHIFT;
inline constexpr std::size_t PAGE_MASK = PAGE_SIZE - 1;

inline constexpr std::size_t PAGES_PER_SEGMENT = SEGMENT_SIZE / PAGE_SIZE;  // 64

// Page index 0 holds the segment header (segment struct + page metadata array);
// it never serves allocations. Pages 1..PAGES_PER_SEGMENT-1 are usable.
inline constexpr std::size_t FIRST_USABLE_PAGE = 1;

// Minimum allocation granularity / alignment. Every block is at least this big
// (so a free block always has room for an intrusive next pointer) and every
// returned pointer is at least this aligned.
inline constexpr std::size_t ALLOC_ALIGN = 16;

// Objects with usable size <= SMALL_SIZE_MAX go through the size-class / page
// machinery. Larger requests are served directly from the OS layer as their own
// segment-aligned region (the "large" path). Chosen so even the largest small
// class still packs several blocks per 64 KiB page.
inline constexpr std::size_t SMALL_SIZE_MAX = 16 * 1024;  // 16 KiB

}  // namespace hm

#endif  // HMALLOC_CONSTANTS_H
