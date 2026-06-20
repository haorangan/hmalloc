/*
 * Minimal, dependency-free test harness shared by hmalloc's unit tests.
 *
 * Usage:
 *   #include "test_util.h"
 *   TEST(my_thing) { CHECK(1 + 1 == 2); CHECK_EQ(foo(), 42); }
 *   int main() { return hm_test::run_all(); }
 *
 * Tests self-register via a static initializer, so adding a TEST() is enough —
 * no central list to maintain. run_all() runs them, prints a summary, and
 * returns non-zero if any CHECK failed so ctest reports pass/fail correctly.
 */
#ifndef HMALLOC_TEST_UTIL_H
#define HMALLOC_TEST_UTIL_H

#include <cstdint>
#include <cstdio>
#include <vector>

namespace hm_test {

inline int &checks() {
  static int c = 0;
  return c;
}
inline int &failures() {
  static int f = 0;
  return f;
}

struct Case {
  const char *name;
  void (*fn)();
};

inline std::vector<Case> &registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
  std::printf("running %zu test case(s)...\n", registry().size());
  for (const Case &c : registry()) {
    int before = failures();
    c.fn();
    bool ok = failures() == before;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", c.name);
  }
  std::printf("%d checks, %d failures\n", checks(), failures());
  if (failures() == 0) std::printf("OK\n");
  return failures() == 0 ? 0 : 1;
}

}  // namespace hm_test

#define TEST(name)                                                       \
  static void hm_test_##name();                                          \
  static ::hm_test::Registrar hm_test_reg_##name(#name, &hm_test_##name); \
  static void hm_test_##name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++::hm_test::checks();                                                \
    if (!(cond)) {                                                        \
      ++::hm_test::failures();                                            \
      std::printf("    FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__,      \
                  #cond);                                                 \
    }                                                                     \
  } while (0)

// CHECK_EQ with operands printed on failure (as signed 64-bit, for ints).
#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    ++::hm_test::checks();                                                     \
    auto _va = (a);                                                            \
    auto _vb = (b);                                                            \
    if (!(_va == _vb)) {                                                       \
      ++::hm_test::failures();                                                 \
      std::printf("    FAIL %s:%d: CHECK_EQ(%s, %s) -> %lld vs %lld\n",        \
                  __FILE__, __LINE__, #a, #b,                                  \
                  static_cast<long long>(_va), static_cast<long long>(_vb));   \
    }                                                                          \
  } while (0)

#endif  // HMALLOC_TEST_UTIL_H
