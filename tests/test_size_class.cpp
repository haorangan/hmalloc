/*
 * Unit tests for size classes (M2): src/size_class.h.
 *
 * Verifies the table is well-formed, that size_class() returns the smallest
 * class that fits (matching a brute-force scan), and that internal
 * fragmentation stays within the designed ~25% bound on the geometric range.
 */
#include "../src/size_class.h"
#include "test_util.h"

#include <cstddef>

using namespace hm;

// Brute-force reference: smallest class whose block size is >= n.
static std::uint32_t ref_class(std::size_t n) {
  for (std::uint32_t c = 0; c < num_size_classes(); ++c)
    if (class_to_size(c) >= n) return c;
  return LARGE_CLASS;
}

TEST(table_well_formed) {
  CHECK(num_size_classes() > 0);
  CHECK(num_size_classes() <= 64);
  CHECK_EQ(class_to_size(0), ALLOC_ALIGN);  // smallest class is the alignment
  // Strictly increasing, every size a multiple of the alignment.
  for (std::uint32_t c = 0; c < num_size_classes(); ++c) {
    CHECK(class_to_size(c) % ALLOC_ALIGN == 0);
    if (c > 0) CHECK(class_to_size(c) > class_to_size(c - 1));
  }
  // Largest small class is exactly SMALL_SIZE_MAX.
  CHECK_EQ(class_to_size(num_size_classes() - 1), SMALL_SIZE_MAX);
}

TEST(matches_bruteforce) {
  for (std::size_t n = 1; n <= SMALL_SIZE_MAX; ++n) {
    std::uint32_t c = size_class(n);
    CHECK(c == ref_class(n));
    // Served size must fit the request, and the previous class must not.
    CHECK(class_to_size(c) >= n);
    if (c > 0) CHECK(class_to_size(c - 1) < n);
  }
}

TEST(large_threshold) {
  CHECK(size_class(SMALL_SIZE_MAX) != LARGE_CLASS);
  CHECK(size_class(SMALL_SIZE_MAX + 1) == LARGE_CLASS);
  CHECK(size_class(1u << 20) == LARGE_CLASS);
  // Zero maps to the smallest class (callers special-case 0 before this).
  CHECK_EQ(size_class(0), 0u);
}

TEST(fragmentation_bound) {
  // Worst-case internal fragmentation: served <= n + n/4 + one alignment step.
  // The n/4 term is the ~25% geometric bound; the +ALLOC_ALIGN covers the dense
  // base octave where spacing is a fixed 16 bytes.
  for (std::size_t n = 1; n <= SMALL_SIZE_MAX; ++n) {
    std::size_t served = size_class_round(n);
    CHECK(served <= n + n / 4 + ALLOC_ALIGN);
  }
}

TEST(round_is_idempotent) {
  // Rounding an already-rounded size yields the same size (it is a class size).
  for (std::uint32_t c = 0; c < num_size_classes(); ++c) {
    std::size_t s = class_to_size(c);
    CHECK_EQ(size_class_round(s), s);
    CHECK_EQ(size_class(s), c);
  }
}

int main() { return hm_test::run_all(); }
