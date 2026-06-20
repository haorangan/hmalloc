/*
 * Unit tests for the OS memory layer (M1): src/os.{h,cpp}.
 *
 * These link the allocator's internal os.cpp directly (not the public API) so
 * we can exercise os_alloc_aligned/os_free/os_purge and the byte accounting.
 */
#include "../src/os.h"
#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace hm;

static bool is_aligned(void *p, std::size_t a) {
  return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}

TEST(page_size_sane) {
  std::size_t ps = os_page_size();
  CHECK(ps >= 4096);
  CHECK((ps & (ps - 1)) == 0);  // power of two
}

TEST(page_aligned_alloc_roundtrip) {
  std::size_t ps = os_page_size();
  std::size_t before = os_stats().bytes_mapped;
  void *p = os_alloc_aligned(ps * 4, ps);
  CHECK(p != nullptr);
  CHECK(is_aligned(p, ps));
  // Memory is usable and the kernel zero-fills it.
  unsigned char *b = static_cast<unsigned char *>(p);
  for (std::size_t i = 0; i < ps * 4; ++i) CHECK(b[i] == 0);
  std::memset(p, 0x5a, ps * 4);
  CHECK(b[ps * 4 - 1] == 0x5a);
  os_free(p, ps * 4);
  CHECK_EQ(os_stats().bytes_mapped, before);  // nothing leaked
}

TEST(over_aligned_alloc) {
  std::size_t ps = os_page_size();
  std::size_t before = os_stats().bytes_mapped;
  // Alignments well above the page size exercise the over-map-and-trim path.
  for (std::size_t align = ps * 2; align <= (std::size_t(1) << 22); align <<= 1) {
    void *p = os_alloc_aligned(ps, align);
    CHECK(p != nullptr);
    CHECK(is_aligned(p, align));
    if (p) {
      std::memset(p, 0xab, ps);  // fully writable
      os_free(p, ps);
    }
  }
  // The trim path must not leak the head/tail slop it unmaps.
  CHECK_EQ(os_stats().bytes_mapped, before);
}

TEST(segment_sized_aligned_alloc) {
  // The real workload: 4 MiB regions aligned to 4 MiB (segment shape).
  const std::size_t seg = std::size_t(1) << 22;
  std::size_t before = os_stats().bytes_mapped;
  std::vector<void *> regions;
  for (int i = 0; i < 8; ++i) {
    void *p = os_alloc_aligned(seg, seg);
    CHECK(p != nullptr);
    CHECK(is_aligned(p, seg));
    regions.push_back(p);
  }
  // Distinct, non-overlapping regions.
  for (size_t i = 0; i < regions.size(); ++i)
    for (size_t j = i + 1; j < regions.size(); ++j) CHECK(regions[i] != regions[j]);
  for (void *p : regions) os_free(p, seg);
  CHECK_EQ(os_stats().bytes_mapped, before);
}

TEST(rejects_bad_arguments) {
  std::size_t ps = os_page_size();
  CHECK(os_alloc_aligned(0, ps) == nullptr);          // zero size
  CHECK(os_alloc_aligned(ps, 0) == nullptr);          // zero alignment
  CHECK(os_alloc_aligned(ps, ps * 3) == nullptr);     // non-pow2 alignment
  CHECK(os_alloc_aligned(ps + 1, ps) == nullptr);     // size not page multiple
  CHECK(os_alloc_aligned(ps, ps + ps / 2) == nullptr);// alignment not page mult
}

TEST(purge_keeps_mapping) {
  std::size_t ps = os_page_size();
  void *p = os_alloc_aligned(ps * 4, ps);
  CHECK(p != nullptr);
  std::memset(p, 0xff, ps * 4);
  std::size_t mapped = os_stats().bytes_mapped;
  CHECK(os_purge(p, ps * 4));
  // Purge drops physical pages but keeps the range mapped & accounted.
  CHECK_EQ(os_stats().bytes_mapped, mapped);
  // Still usable after purge.
  std::memset(p, 0x11, ps * 4);
  CHECK(static_cast<unsigned char *>(p)[0] == 0x11);
  os_free(p, ps * 4);
}

TEST(accounting_counts) {
  std::size_t ps = os_page_size();
  OsStats a = os_stats();
  void *p = os_alloc_aligned(ps, ps);
  OsStats b = os_stats();
  CHECK(b.map_calls == a.map_calls + 1);
  CHECK(b.bytes_mapped == a.bytes_mapped + ps);
  CHECK(b.peak_mapped >= b.bytes_mapped);
  os_free(p, ps);
  OsStats c = os_stats();
  CHECK(c.unmap_calls == b.unmap_calls + 1);
  CHECK_EQ(c.bytes_mapped, a.bytes_mapped);
}

int main() { return hm_test::run_all(); }
