/*
 * Correctness + stress tests for the real allocator via the public API.
 *
 * These exercise the size-class/page/segment machinery: spanning every class,
 * forcing multiple pages and segments, large and huge allocations, aligned
 * allocations, calloc zeroing of recycled memory, realloc growth/shrink, and a
 * randomized churn workload. Each live allocation carries a unique canary so any
 * overlap or use-after-free corruption is caught on verification.
 */
#include "hmalloc/hmalloc.h"
#include "test_util.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
std::uint64_t xrand() {
  std::uint64_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_rng = x;
  return x;
}

bool is_aligned(void *p, std::size_t a) {
  return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}

// A live allocation tracked with a canary so corruption/overlap is detectable.
struct Live {
  unsigned char *p = nullptr;
  std::size_t size = 0;
  unsigned char seed = 0;
};

unsigned char seed_for(std::uint64_t id) {
  return static_cast<unsigned char>((id * 1099511628211ull) & 0xff);
}

void fill(Live &a) {
  std::memset(a.p, a.seed, a.size);
}
bool verify(const Live &a) {
  for (std::size_t i = 0; i < a.size; ++i)
    if (a.p[i] != a.seed) return false;
  return true;
}

}  // namespace

TEST(spans_every_class_and_large) {
  // Sizes touching each small class boundary plus several large/huge sizes.
  std::vector<std::size_t> sizes;
  for (std::size_t s = 1; s <= 16 * 1024; s = s + 1 + s / 7) sizes.push_back(s);
  for (std::size_t s : {16385u, 17000u, 65536u, 100000u, 1u << 20})
    sizes.push_back(s);
  sizes.push_back(5u << 20);  // multi-segment huge object

  for (std::size_t s : sizes) {
    void *p = hm_malloc(s);
    CHECK(p != nullptr);
    if (!p) continue;
    CHECK(is_aligned(p, 16));
    CHECK(hm_usable_size(p) >= s);
    std::memset(p, 0xab, s);  // full write must stay in bounds
    CHECK(static_cast<unsigned char *>(p)[s - 1] == 0xab);
    hm_free(p);
  }
}

TEST(no_overlap_or_corruption) {
  // Many same-class allocations, each with its own canary; if any two overlap,
  // their canaries collide and verification fails.
  const int N = 20000;
  std::vector<Live> live(N);
  for (int i = 0; i < N; ++i) {
    live[i].size = 24;  // class of 32 bytes
    live[i].seed = seed_for(static_cast<std::uint64_t>(i) + 1);
    live[i].p = static_cast<unsigned char *>(hm_malloc(live[i].size));
    CHECK(live[i].p != nullptr);
    fill(live[i]);
  }
  int bad = 0;
  for (int i = 0; i < N; ++i)
    if (!verify(live[i])) ++bad;
  CHECK_EQ(bad, 0);
  for (int i = 0; i < N; ++i) hm_free(live[i].p);
}

TEST(forces_multiple_segments) {
  // Class 64 packs 1024 blocks/page * 63 pages = 64512 per 4 MiB segment.
  // Allocating well past that forces several segments to be mapped.
  const int N = 200000;
  std::vector<void *> ps(N);
  for (int i = 0; i < N; ++i) {
    ps[i] = hm_malloc(64);
    CHECK(ps[i] != nullptr);
    static_cast<unsigned char *>(ps[i])[0] = static_cast<unsigned char>(i);
  }
  for (int i = 0; i < N; ++i) hm_free(ps[i]);
  // Reallocate the same volume: pages/segments must be reusable, not re-grown
  // unboundedly. (Bounded-ness is asserted precisely in test_stats once M8 lands.)
  for (int i = 0; i < N; ++i) {
    ps[i] = hm_malloc(64);
    CHECK(ps[i] != nullptr);
  }
  for (int i = 0; i < N; ++i) hm_free(ps[i]);
}

TEST(neighbors_isolated) {
  // Writing the full usable size of one block must not touch its neighbor.
  std::vector<Live> live(5000);
  for (std::size_t i = 0; i < live.size(); ++i) {
    std::size_t req = 1 + (i % 512);
    live[i].p = static_cast<unsigned char *>(hm_malloc(req));
    CHECK(live[i].p != nullptr);
    live[i].size = hm_usable_size(live[i].p);  // write the whole usable region
    live[i].seed = seed_for(i + 7);
    fill(live[i]);
  }
  int bad = 0;
  for (const Live &a : live)
    if (!verify(a)) ++bad;
  CHECK_EQ(bad, 0);
  for (const Live &a : live) hm_free(a.p);
}

TEST(aligned_alloc_variants) {
  const std::size_t aligns[] = {16, 32, 64, 128, 256, 512, 4096, 65536, 1u << 20};
  for (std::size_t a : aligns) {
    for (std::size_t s : {std::size_t(1), a / 2 + 1, a * 3 + 7, std::size_t(100000)}) {
      if (s == 0) continue;
      void *p = hm_aligned_alloc(a, s);
      CHECK(p != nullptr);
      if (!p) continue;
      CHECK(is_aligned(p, a));
      CHECK(hm_usable_size(p) >= s);
      std::memset(p, 0x5a, s);
      CHECK(static_cast<unsigned char *>(p)[s - 1] == 0x5a);
      hm_free(p);
    }
  }
  CHECK(hm_aligned_alloc(24, 64) == nullptr);  // non-power-of-two rejected
  CHECK(hm_aligned_alloc(0, 64) == nullptr);
}

TEST(calloc_zeroes_recycled_memory) {
  // Dirty a block, free it, then calloc the same size: it likely reuses that
  // block and must come back zeroed.
  for (int rep = 0; rep < 1000; ++rep) {
    void *d = hm_malloc(200);
    CHECK(d != nullptr);
    std::memset(d, 0xff, 200);
    hm_free(d);
    auto *z = static_cast<unsigned char *>(hm_calloc(50, 4));  // 200 bytes
    CHECK(z != nullptr);
    bool all_zero = true;
    for (int i = 0; i < 200; ++i)
      if (z[i] != 0) all_zero = false;
    CHECK(all_zero);
    hm_free(z);
  }
  CHECK(hm_calloc(SIZE_MAX, 2) == nullptr);  // overflow detected
  CHECK(hm_calloc(0, 8) == nullptr);
}

TEST(realloc_preserves_contents) {
  // null -> malloc
  auto *p = static_cast<unsigned char *>(hm_realloc(nullptr, 64));
  CHECK(p != nullptr);
  for (int i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i);

  // grow within small classes
  p = static_cast<unsigned char *>(hm_realloc(p, 4000));
  CHECK(p != nullptr);
  bool ok = true;
  for (int i = 0; i < 64; ++i) ok = ok && p[i] == static_cast<unsigned char>(i);
  CHECK(ok);

  // grow small -> large (crosses the page/segment boundary)
  p = static_cast<unsigned char *>(hm_realloc(p, 200000));
  CHECK(p != nullptr);
  ok = true;
  for (int i = 0; i < 64; ++i) ok = ok && p[i] == static_cast<unsigned char>(i);
  CHECK(ok);

  // shrink large -> small
  p = static_cast<unsigned char *>(hm_realloc(p, 32));
  CHECK(p != nullptr);
  ok = true;
  for (int i = 0; i < 32; ++i) ok = ok && p[i] == static_cast<unsigned char>(i);
  CHECK(ok);

  CHECK(hm_realloc(p, 0) == nullptr);  // realloc to 0 frees, returns null
}

TEST(randomized_churn) {
  // Keep up to N live allocations, randomly allocating and freeing while
  // verifying canaries. Mixes small and (occasionally) large sizes.
  const std::size_t N = 4000;
  std::vector<Live> live(N);
  std::uint64_t id = 0;
  for (int step = 0; step < 400000; ++step) {
    std::size_t slot = xrand() % N;
    if (live[slot].p == nullptr) {
      std::size_t s = (xrand() % 32 == 0) ? (20000 + xrand() % 80000)
                                          : (1 + xrand() % 1500);
      Live a;
      a.size = s;
      a.seed = seed_for(++id);
      a.p = static_cast<unsigned char *>(hm_malloc(s));
      CHECK(a.p != nullptr);
      if (a.p) {
        fill(a);
        live[slot] = a;
      }
    } else {
      CHECK(verify(live[slot]));
      hm_free(live[slot].p);
      live[slot] = Live{};
    }
  }
  for (std::size_t i = 0; i < N; ++i)
    if (live[i].p) {
      CHECK(verify(live[i]));
      hm_free(live[i].p);
    }
}

TEST(huge_sizes_fail_cleanly) {
  // Near-SIZE_MAX requests must return null, not overflow internal size math
  // (e.g. the large-path round-up or the OS over-map). Regression for the
  // os_alloc_aligned over-map overflow found in review.
  CHECK(hm_malloc(SIZE_MAX) == nullptr);
  CHECK(hm_malloc(SIZE_MAX - 4096) == nullptr);
  CHECK(hm_malloc(SIZE_MAX - (static_cast<std::size_t>(1) << 22) + 1) == nullptr);
  CHECK(hm_calloc(1, SIZE_MAX) == nullptr);
  CHECK(hm_aligned_alloc(64, SIZE_MAX) == nullptr);
  // A modest large allocation still works right after the failures.
  void *p = hm_malloc(100000);
  CHECK(p != nullptr);
  hm_free(p);
}

int main() { return hm_test::run_all(); }
