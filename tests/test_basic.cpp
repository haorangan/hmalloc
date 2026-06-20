/*
 * Basic correctness tests for the hmalloc public API.
 *
 * Dependency-free: a tiny CHECK macro tracks failures and the process exits
 * non-zero if any check fails, so `ctest` reports pass/fail correctly.
 */
#include "hmalloc/hmalloc.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                  \
  } while (0)

static bool is_aligned(void *p, size_t a) {
  return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

static void test_malloc_basic() {
  const size_t sizes[] = {1, 8, 15, 16, 17, 64, 100, 4096, 100000};
  for (size_t s : sizes) {
    void *p = hm_malloc(s);
    CHECK(p != nullptr);
    if (!p) continue;
    CHECK(is_aligned(p, alignof(max_align_t)));
    CHECK(hm_usable_size(p) >= s);
    std::memset(p, 0xab, s); /* full write must not corrupt anything */
    CHECK(static_cast<unsigned char *>(p)[s - 1] == 0xab);
    hm_free(p);
  }
  CHECK(hm_malloc(0) == nullptr);
}

static void test_free_null() {
  hm_free(nullptr); /* must be a no-op, no crash */
  CHECK(true);
}

static void test_calloc() {
  const size_t n = 256, sz = 8;
  unsigned char *p = static_cast<unsigned char *>(hm_calloc(n, sz));
  CHECK(p != nullptr);
  if (p) {
    bool all_zero = true;
    for (size_t i = 0; i < n * sz; ++i)
      if (p[i] != 0) all_zero = false;
    CHECK(all_zero);
    hm_free(p);
  }
  /* Overflow must be detected, not wrap. */
  CHECK(hm_calloc(SIZE_MAX, 2) == nullptr);
}

static void test_realloc() {
  /* realloc(NULL, n) behaves like malloc. */
  unsigned char *p = static_cast<unsigned char *>(hm_realloc(nullptr, 32));
  CHECK(p != nullptr);
  for (int i = 0; i < 32; ++i) p[i] = static_cast<unsigned char>(i);

  /* Growing preserves existing bytes. */
  p = static_cast<unsigned char *>(hm_realloc(p, 4096));
  CHECK(p != nullptr);
  bool preserved = true;
  for (int i = 0; i < 32; ++i)
    if (p[i] != static_cast<unsigned char>(i)) preserved = false;
  CHECK(preserved);

  /* realloc(ptr, 0) frees and returns NULL. */
  CHECK(hm_realloc(p, 0) == nullptr);
}

static void test_aligned_alloc() {
  for (size_t a = sizeof(void *); a <= 4096; a <<= 1) {
    void *p = hm_aligned_alloc(a, a * 3);
    CHECK(p != nullptr);
    if (p) {
      CHECK(is_aligned(p, a));
      hm_free(p);
    }
  }
  /* Non-power-of-two alignment is rejected. */
  CHECK(hm_aligned_alloc(24, 64) == nullptr);
}

int main() {
  std::printf("running hmalloc basic tests...\n");
  test_malloc_basic();
  test_free_null();
  test_calloc();
  test_realloc();
  test_aligned_alloc();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("OK\n");
  return g_failures == 0 ? 0 : 1;
}
