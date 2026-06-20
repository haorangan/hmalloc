/*
 * hmalloc benchmark harness (M7).
 *
 * Measures hmalloc against the system allocator across the workloads from
 * DESIGN.md §10:
 *
 *   single    — single-thread throughput, fixed object size
 *   mixed     — single-thread throughput, mixed sizes (realistic distribution)
 *   scaling   — N independent threads, 1..cores, throughput + scaling efficiency
 *   prodcons  — producer threads allocate, consumer threads free (cross-thread)
 *   frag      — long-running mixed-size churn; reports resident set size (RSS)
 *
 * Every "op" is one malloc + one free. Allocations are touched so dead-store
 * elimination can't delete them and pages are actually resident. To compare
 * other allocators, run the binary under LD_PRELOAD / DYLD_INSERT_LIBRARIES.
 *
 * Usage: ./bench [scenario]      (scenario defaults to "all")
 */
#include "hmalloc/hmalloc.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
double ns_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
}

volatile unsigned char g_sink = 0;

// Cheap per-thread PRNG (no shared state, no locks).
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

std::size_t rss_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    return info.resident_size;
  return 0;
#elif defined(__linux__)
  std::size_t rss = 0;
  if (FILE *f = std::fopen("/proc/self/statm", "r")) {
    long pages = 0;
    if (std::fscanf(f, "%*s %ld", &pages) == 1)
      rss = static_cast<std::size_t>(pages) * sysconf(_SC_PAGESIZE);
    std::fclose(f);
  }
  return rss;
#else
  return 0;
#endif
}

// An allocator is a pair of malloc/free callables. Two instances: system & hm.
struct SysAlloc {
  static void *a(std::size_t n) { return std::malloc(n); }
  static void f(void *p) { std::free(p); }
  static const char *name() { return "system"; }
};
struct HmAlloc {
  static void *a(std::size_t n) { return hm_malloc(n); }
  static void f(void *p) { hm_free(p); }
  static const char *name() { return "hmalloc"; }
};

// One malloc+free per op, with a touch, over `iters` ops in batches of `batch`.
template <class A>
double single_thread(std::size_t iters, std::size_t batch, std::size_t obj) {
  std::vector<void *> live(batch, nullptr);
  for (std::size_t i = 0; i < batch; ++i) {  // warm up first-touch faults
    void *p = A::a(obj);
    if (p) std::memset(p, 1, obj);
    A::f(p);
  }
  std::size_t ops = 0;
  auto t0 = Clock::now();
  while (ops < iters) {
    for (std::size_t i = 0; i < batch; ++i) {
      live[i] = A::a(obj);
      if (live[i]) static_cast<unsigned char *>(live[i])[0] = 0xcd;
    }
    for (std::size_t i = 0; i < batch; ++i) {
      if (live[i]) g_sink += static_cast<unsigned char *>(live[i])[0];
      A::f(live[i]);
    }
    ops += batch;
  }
  return ns_since(t0) / static_cast<double>(ops);
}

// Mixed sizes drawn from a heavy-small distribution (most allocs are tiny).
template <class A>
double single_thread_mixed(std::size_t iters, std::size_t batch) {
  std::vector<void *> live(batch, nullptr);
  std::vector<std::size_t> sizes(batch);
  Rng rng(12345);
  auto pick = [&]() -> std::size_t {
    std::uint64_t r = rng.next();
    if (r % 16 == 0) return 1024 + (r % 8192);  // ~6% medium
    return 8 + (r % 248);                        // mostly 8..256 bytes
  };
  std::size_t ops = 0;
  auto t0 = Clock::now();
  while (ops < iters) {
    for (std::size_t i = 0; i < batch; ++i) {
      sizes[i] = pick();
      live[i] = A::a(sizes[i]);
      if (live[i]) static_cast<unsigned char *>(live[i])[0] = 0xcd;
    }
    for (std::size_t i = 0; i < batch; ++i) {
      if (live[i]) g_sink += static_cast<unsigned char *>(live[i])[0];
      A::f(live[i]);
    }
    ops += batch;
  }
  return ns_since(t0) / static_cast<double>(ops);
}

void report(const char *label, double ns_per_op) {
  std::printf("  %-18s %10.2f ns/op   %8.2f Mops/s\n", label, ns_per_op,
              1000.0 / ns_per_op);
}

// --- scenarios -------------------------------------------------------------

void scenario_single(std::size_t iters, std::size_t obj) {
  std::printf("[single] fixed %zu-byte objects, %zu ops\n", obj, iters);
  double s = single_thread<SysAlloc>(iters, 256, obj);
  double h = single_thread<HmAlloc>(iters, 256, obj);
  report("system malloc", s);
  report("hmalloc", h);
  std::printf("  speedup: %.2fx\n\n", s / h);
}

void scenario_mixed(std::size_t iters) {
  std::printf("[mixed] mixed-size single-thread, %zu ops\n", iters);
  double s = single_thread_mixed<SysAlloc>(iters, 256);
  double h = single_thread_mixed<HmAlloc>(iters, 256);
  report("system malloc", s);
  report("hmalloc", h);
  std::printf("  speedup: %.2fx\n\n", s / h);
}

// Each of T threads runs an independent fixed-size churn; returns total Mops/s.
template <class A>
double scaling_run(unsigned T, std::size_t per_thread, std::size_t obj) {
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  auto t0p = Clock::time_point{};
  std::atomic<unsigned> ready{0};
  auto worker = [&](unsigned) {
    std::vector<void *> live(64, nullptr);
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) {
    }
    for (std::size_t i = 0; i < per_thread; i += 64) {
      for (int j = 0; j < 64; ++j) {
        live[j] = A::a(obj);
        if (live[j]) static_cast<unsigned char *>(live[j])[0] = 0xcd;
      }
      for (int j = 0; j < 64; ++j) {
        if (live[j]) g_sink += static_cast<unsigned char *>(live[j])[0];
        A::f(live[j]);
      }
    }
  };
  for (unsigned t = 0; t < T; ++t) ts.emplace_back(worker, t);
  while (ready.load(std::memory_order_acquire) < T) {
  }
  auto t0 = Clock::now();
  (void)t0p;
  go.store(true, std::memory_order_release);
  for (auto &th : ts) th.join();
  double ns = ns_since(t0);
  double ops = static_cast<double>(T) * static_cast<double>(per_thread);
  return ops / (ns / 1000.0);  // Mops/s
}

void scenario_scaling(std::size_t per_thread, std::size_t obj) {
  unsigned cores = std::thread::hardware_concurrency();
  if (cores == 0) cores = 4;
  std::printf("[scaling] %zu ops/thread, %zu-byte objects, up to %u threads\n",
              per_thread, obj, cores);
  std::printf("  %-8s %14s %14s %10s\n", "threads", "system Mops/s",
              "hmalloc Mops/s", "hm scale");
  double base = 0;
  for (unsigned T = 1; T <= cores; T <<= 1) {
    double s = scaling_run<SysAlloc>(T, per_thread, obj);
    double h = scaling_run<HmAlloc>(T, per_thread, obj);
    if (T == 1) base = h;
    std::printf("  %-8u %14.2f %14.2f %9.2fx\n", T, s, h, h / base);
    if (T != cores && (T << 1) > cores) T = cores >> 1;  // also hit exact cores
  }
  std::printf("\n");
}

// Producers allocate + enqueue; consumers dequeue + free. The free lands on a
// different thread than the alloc: hmalloc's cross-thread (thread_free) path.
template <class A>
double prodcons_run(unsigned P, unsigned C, std::size_t per_producer,
                    std::size_t obj) {
  std::mutex m;
  std::queue<void *> q;
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> consumed{0};
  const std::uint64_t total = static_cast<std::uint64_t>(P) * per_producer;

  std::vector<std::thread> threads;
  auto t0 = Clock::now();
  for (unsigned p = 0; p < P; ++p) {
    threads.emplace_back([&]() {
      for (std::size_t i = 0; i < per_producer; ++i) {
        void *ptr = A::a(obj);
        if (ptr) static_cast<unsigned char *>(ptr)[0] = 0xcd;
        std::lock_guard<std::mutex> lk(m);
        q.push(ptr);
      }
    });
  }
  for (unsigned c = 0; c < C; ++c) {
    threads.emplace_back([&]() {
      for (;;) {
        void *ptr = nullptr;
        {
          std::lock_guard<std::mutex> lk(m);
          if (!q.empty()) {
            ptr = q.front();
            q.pop();
          }
        }
        if (ptr) {
          g_sink += static_cast<unsigned char *>(ptr)[0];
          A::f(ptr);
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else if (done.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lk(m);
          if (q.empty()) break;
        }
      }
    });
  }
  for (unsigned p = 0; p < P; ++p) threads[p].join();
  done.store(true, std::memory_order_release);
  for (unsigned c = 0; c < C; ++c) threads[P + c].join();
  double ns = ns_since(t0);
  return static_cast<double>(total) / (ns / 1000.0);  // Mops/s
}

void scenario_prodcons(std::size_t per_producer, std::size_t obj) {
  unsigned cores = std::thread::hardware_concurrency();
  if (cores < 2) cores = 2;
  unsigned P = cores / 2, C = cores - P;
  std::printf("[prodcons] %u producers / %u consumers, %zu allocs/producer, "
              "%zu-byte objects\n",
              P, C, per_producer, obj);
  double s = prodcons_run<SysAlloc>(P, C, per_producer, obj);
  double h = prodcons_run<HmAlloc>(P, C, per_producer, obj);
  std::printf("  %-18s %8.2f Mops/s\n", "system malloc", s);
  std::printf("  %-18s %8.2f Mops/s\n", "hmalloc", h);
  std::printf("  speedup: %.2fx\n\n", h / s);
}

// Hold ~live_set objects of random sizes, replacing them at random for many
// rounds, sampling RSS. Shows whether memory is returned and how fragmentation
// behaves under sustained churn.
template <class A>
void frag_run(std::size_t live_set, std::size_t rounds, std::size_t &peak,
              std::size_t &final_rss) {
  std::vector<void *> live(live_set, nullptr);
  std::vector<std::size_t> sz(live_set, 0);
  Rng rng(99);
  peak = 0;
  for (std::size_t r = 0; r < rounds; ++r) {
    std::size_t i = rng.next() % live_set;
    if (live[i]) {
      A::f(live[i]);
      live[i] = nullptr;
    }
    std::uint64_t v = rng.next();
    std::size_t s = (v % 64 == 0) ? (8192 + v % 65536) : (8 + v % 1024);
    live[i] = A::a(s);
    sz[i] = s;
    if (live[i]) std::memset(live[i], 0xab, s);
    if ((r & 0xffff) == 0) {
      std::size_t rss = rss_bytes();
      if (rss > peak) peak = rss;
    }
  }
  final_rss = rss_bytes();
  if (final_rss > peak) peak = final_rss;
  for (std::size_t i = 0; i < live_set; ++i) A::f(live[i]);
}

void scenario_frag(std::size_t live_set, std::size_t rounds) {
  std::printf("[frag] %zu live objects, %zu churn rounds (RSS in MiB)\n",
              live_set, rounds);
  std::size_t sp = 0, sf = 0, hp = 0, hf = 0;
  frag_run<SysAlloc>(live_set, rounds, sp, sf);
  frag_run<HmAlloc>(live_set, rounds, hp, hf);
  auto mib = [](std::size_t b) { return static_cast<double>(b) / (1024 * 1024); };
  std::printf("  %-18s peak %8.1f   final %8.1f\n", "system malloc", mib(sp), mib(sf));
  std::printf("  %-18s peak %8.1f   final %8.1f\n", "hmalloc", mib(hp), mib(hf));
  std::printf("\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::string which = (argc > 1) ? argv[1] : "all";
  bool all = (which == "all");

  std::printf("hmalloc benchmarks (%u hardware threads)\n\n",
              std::thread::hardware_concurrency());

  if (all || which == "single") scenario_single(20'000'000, 64);
  if (all || which == "mixed") scenario_mixed(10'000'000);
  if (all || which == "scaling") scenario_scaling(2'000'000, 64);
  if (all || which == "prodcons") scenario_prodcons(2'000'000, 64);
  if (all || which == "frag") scenario_frag(200'000, 4'000'000);
  return 0;
}
