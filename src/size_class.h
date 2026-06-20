/*
 * hmalloc — size classes (M2).
 *
 * Requests are rounded up to one of a fixed set of size classes so that a page
 * only ever serves one block size and every free slot in it is interchangeable.
 *
 * Spacing is geometric with four steps per power-of-two octave:
 *
 *     16, 32, 48, 64,  80, 96, 112, 128,  160, 192, 224, 256,  320, ... , 16384
 *
 * This bounds worst-case internal fragmentation to ~25% (an object just over a
 * class boundary wastes at most one step = 1/4 of its octave) while keeping the
 * class count small (~36 up to SMALL_SIZE_MAX).
 *
 * Everything here is header-only `constexpr` so the hot path inlines a single
 * table lookup. `size_class(n)` is O(1): a direct index into a byte table over
 * the dense small range.
 */
#ifndef HMALLOC_SIZE_CLASS_H
#define HMALLOC_SIZE_CLASS_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "constants.h"

namespace hm {

// Sentinel returned by size_class() for requests above SMALL_SIZE_MAX: they do
// not use the size-class machinery and go through the large/OS path instead.
inline constexpr std::uint32_t LARGE_CLASS = 0xFFFFFFFFu;

namespace detail {

inline constexpr std::size_t kMaxClasses = 64;
inline constexpr std::size_t kLookupEntries = SMALL_SIZE_MAX / ALLOC_ALIGN;  // 1024

struct SizeClasses {
  std::array<std::uint32_t, kMaxClasses> size{};  // class index -> block size
  std::uint32_t count = 0;
  // size_to_class[(n-1)/ALLOC_ALIGN] = class index, for n in [1, SMALL_SIZE_MAX].
  std::array<std::uint8_t, kLookupEntries> lookup{};
};

// Build the class table and the reverse lookup at compile time.
inline constexpr SizeClasses build_size_classes() {
  SizeClasses t{};

  // Dense base octave [16, 64]: 16, 32, 48, 64 (step = ALLOC_ALIGN).
  for (std::uint32_t s = ALLOC_ALIGN; s <= 64; s += ALLOC_ALIGN)
    t.size[t.count++] = s;

  // Geometric octaves (2^k, 2^(k+1)] for k >= 6, four steps of 2^(k-2) each, up
  // to SMALL_SIZE_MAX. The last step of an octave equals 2^(k+1).
  for (std::size_t k = 6; (std::size_t(1) << k) < SMALL_SIZE_MAX; ++k) {
    const std::uint32_t base = std::uint32_t(1) << k;
    const std::uint32_t step = base >> 2;
    for (int i = 1; i <= 4; ++i) {
      const std::uint32_t s = base + std::uint32_t(i) * step;
      if (s <= SMALL_SIZE_MAX) t.size[t.count++] = s;
    }
  }

  // Reverse lookup: walk class sizes in order, filling every bucket they cover.
  std::uint32_t cls = 0;
  for (std::size_t i = 0; i < kLookupEntries; ++i) {
    const std::uint32_t n = static_cast<std::uint32_t>((i + 1) * ALLOC_ALIGN);
    while (cls + 1 < t.count && t.size[cls] < n) ++cls;
    t.lookup[i] = static_cast<std::uint8_t>(cls);
  }
  return t;
}

inline constexpr SizeClasses kTable = build_size_classes();

}  // namespace detail

// Number of small size classes. Available both as a function and as a constant
// usable for array sizing (e.g. Heap::pages[kNumSizeClasses]).
inline constexpr std::uint32_t kNumSizeClasses = detail::kTable.count;
inline constexpr std::uint32_t num_size_classes() { return kNumSizeClasses; }

// Block size served by class index `c`.
inline constexpr std::uint32_t class_to_size(std::uint32_t c) {
  return detail::kTable.size[c];
}

// Map a request to a size class, or LARGE_CLASS if it exceeds SMALL_SIZE_MAX.
// A request of 0 maps to the smallest class (callers handle 0 before this; this
// keeps the function total and branch-light).
inline constexpr std::uint32_t size_class(std::size_t n) {
  if (n > SMALL_SIZE_MAX) return LARGE_CLASS;
  if (n == 0) n = 1;
  return detail::kTable.lookup[(n - 1) / ALLOC_ALIGN];
}

// Round a small request up to the block size it will actually be served at.
inline constexpr std::size_t size_class_round(std::size_t n) {
  return class_to_size(size_class(n));
}

}  // namespace hm

#endif  // HMALLOC_SIZE_CLASS_H
