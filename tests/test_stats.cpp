/*
 * Tests for hm_stats() (M8). Compiled with -DHMALLOC_STATS (see CMakeLists) so
 * the call-count fields are populated and the fast-path hit rate is checked.
 *
 * Doubles as the precise recycling test promised by test_alloc: after a spike of
 * allocations is freed, pages must return to the central pool and idle segments
 * must be unmapped, so pages_in_use and bytes_reserved fall back down.
 */
#include "hmalloc/hmalloc.h"
#include "test_util.h"

#include <cstddef>
#include <vector>

TEST(recycling_returns_memory) {
  hm_stats_t base = hm_stats();

  const int N = 200000;  // ~196 pages of class 64, several 4 MiB segments
  std::vector<void *> ps;
  ps.reserve(N);
  for (int i = 0; i < N; ++i) {
    void *p = hm_malloc(64);
    CHECK(p != nullptr);
    ps.push_back(p);
  }

  hm_stats_t peak = hm_stats();
  CHECK(peak.pages_in_use > base.pages_in_use + 100);
  CHECK(peak.segments_mapped >= 2);
  CHECK(peak.bytes_reserved > base.bytes_reserved);

  for (void *p : ps) hm_free(p);

  hm_stats_t after = hm_stats();
  // All but the retained active head page should be back; idle segments unmapped.
  CHECK(after.pages_in_use <= base.pages_in_use + 2);
  CHECK(after.bytes_reserved < peak.bytes_reserved);
}

TEST(large_accounting) {
  hm_stats_t base = hm_stats();
  std::vector<void *> ps;
  for (int i = 0; i < 10; ++i) {
    void *p = hm_malloc(100000);  // large path
    CHECK(p != nullptr);
    ps.push_back(p);
  }
  hm_stats_t mid = hm_stats();
  CHECK_EQ(mid.large_allocations, base.large_allocations + 10);
  CHECK(mid.large_bytes > base.large_bytes);

  for (void *p : ps) hm_free(p);
  hm_stats_t after = hm_stats();
  CHECK_EQ(after.large_allocations, base.large_allocations);
  CHECK_EQ(after.large_bytes, base.large_bytes);
}

TEST(fast_path_dominates) {
#ifdef HMALLOC_STATS
  hm_stats_t a = hm_stats();
  const int N = 100000;
  for (int i = 0; i < N; ++i) {
    void *p = hm_malloc(48);
    hm_free(p);
  }
  hm_stats_t b = hm_stats();
  unsigned long long dm = b.malloc_count - a.malloc_count;
  unsigned long long df = b.fast_path_count - a.fast_path_count;
  CHECK(dm >= static_cast<unsigned long long>(N));
  // Balanced alloc/free on one class should hit the fast path almost always.
  CHECK(df * 100 >= dm * 95);
#else
  CHECK(true);  // counters disabled in this build
#endif
}

int main() { return hm_test::run_all(); }
