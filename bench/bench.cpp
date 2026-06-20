/*
 * hmalloc benchmark harness.
 *
 * Reports single-threaded throughput for hmalloc against the system allocator
 * so every milestone can be measured the moment it lands. Multi-threaded,
 * producer/consumer, and fragmentation benchmarks arrive in M7 (see DESIGN.md).
 *
 * Usage: ./bench [iterations] [batch] [object_size]
 */
#include "hmalloc/hmalloc.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Prevent the optimizer from eliding allocations we never "use".
volatile unsigned char g_sink = 0;

struct Result {
  double ns_per_op;
  double mops;
};

// One "op" = one malloc + one free. We allocate `batch` objects, touch them,
// then free them, repeating until `iterations` ops have run. Touching defeats
// dead-store elimination and forces the pages to be resident.
template <typename Alloc, typename Free>
Result run(const char *name, size_t iterations, size_t batch, size_t obj_size,
           Alloc alloc, Free dealloc) {
  std::vector<void *> live(batch, nullptr);
  size_t ops = 0;

  // Warm up so first-touch page faults don't dominate the measurement.
  for (size_t i = 0; i < batch; ++i) {
    void *p = alloc(obj_size);
    if (p) std::memset(p, 1, obj_size);
    dealloc(p);
  }

  auto start = Clock::now();
  while (ops < iterations) {
    for (size_t i = 0; i < batch; ++i) {
      live[i] = alloc(obj_size);
      if (live[i]) static_cast<unsigned char *>(live[i])[0] = 0xcd;
    }
    for (size_t i = 0; i < batch; ++i) {
      if (live[i]) g_sink += static_cast<unsigned char *>(live[i])[0];
      dealloc(live[i]);
    }
    ops += batch;
  }
  auto end = Clock::now();

  double ns = std::chrono::duration<double, std::nano>(end - start).count();
  Result r{ns / static_cast<double>(ops),
           static_cast<double>(ops) / (ns / 1e3)};
  std::printf("  %-16s %10.2f ns/op   %8.2f Mops/s\n", name, r.ns_per_op,
              r.mops);
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  size_t iterations = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
  size_t batch = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 256;
  size_t obj_size = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 64;

  std::printf("hmalloc bench: iterations=%zu batch=%zu obj_size=%zu\n",
              iterations, batch, obj_size);

  Result sys = run(
      "system malloc", iterations, batch, obj_size,
      [](size_t s) { return std::malloc(s); }, [](void *p) { std::free(p); });

  Result hm = run(
      "hmalloc", iterations, batch, obj_size, [](size_t s) { return hm_malloc(s); },
      [](void *p) { hm_free(p); });

  std::printf("  speedup vs system: %.2fx\n", sys.ns_per_op / hm.ns_per_op);
  return 0;
}
