/*
 * Multithreaded correctness tests — the cross-thread paths (M5).
 *
 * Run these under ThreadSanitizer (cmake -DHM_SANITIZE=thread) to check the
 * lock-free thread_free list's memory ordering, not just functional behavior:
 *
 *   - independent_threads : per-thread heaps, concurrent central acquisition.
 *   - producer_consumer   : objects allocated on one thread, freed on another
 *                           (stresses the atomic thread_free push + owner collect).
 *   - free_only_threads   : threads that only ever free remote allocations
 *                           (never build a heap of their own).
 *
 * Every object carries a canary keyed to its identity, verified before free, so
 * any torn handoff, overlap, or lost/duplicated block is caught.
 */
#include "hmalloc/hmalloc.h"
#include "test_util.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace {

unsigned char seed_for(std::uint64_t id) {
  return static_cast<unsigned char>((id * 1099511628211ull + 7) & 0xff);
}

std::size_t size_for(std::uint64_t id) {
  // Spread across small classes and the large path.
  std::size_t s = static_cast<std::size_t>(8 + (id % 2048));
  if (id % 64 == 0) s = 20000 + (id % 50000);
  return s;
}

struct Item {
  unsigned char *p;
  std::size_t size;
  unsigned char seed;
};

Item make_item(std::uint64_t id) {
  Item it;
  it.size = size_for(id);
  it.seed = seed_for(id);
  it.p = static_cast<unsigned char *>(hm_malloc(it.size));
  if (it.p) std::memset(it.p, it.seed, it.size);
  return it;
}

bool check_item(const Item &it) {
  if (!it.p) return false;
  for (std::size_t i = 0; i < it.size; ++i)
    if (it.p[i] != it.seed) return false;
  return true;
}

unsigned hw_threads() {
  unsigned n = std::thread::hardware_concurrency();
  return (n == 0) ? 4 : n;
}

}  // namespace

TEST(independent_threads) {
  const unsigned T = hw_threads();
  const int per = 50000;
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < T; ++t) {
    ts.emplace_back([&, t]() {
      std::vector<Item> live;
      live.reserve(2048);
      std::uint64_t id = static_cast<std::uint64_t>(t) << 40;
      for (int i = 0; i < per; ++i) {
        if (live.size() < 2048 && (i & 1)) {
          Item it = make_item(++id);
          if (!it.p) ++bad;
          live.push_back(it);
        } else if (!live.empty()) {
          Item it = live.back();
          live.pop_back();
          if (!check_item(it)) ++bad;
          hm_free(it.p);
        }
      }
      for (const Item &it : live) {
        if (!check_item(it)) ++bad;
        hm_free(it.p);
      }
    });
  }
  for (auto &th : ts) th.join();
  CHECK_EQ(bad.load(), 0);
}

TEST(producer_consumer) {
  // Producers allocate + fill + enqueue; consumers dequeue + verify + free.
  // The free happens on a different thread than the alloc -> remote-free path.
  const unsigned P = (hw_threads() + 1) / 2;
  const unsigned C = P;
  const std::uint64_t per_producer = 200000;

  std::mutex m;
  std::queue<Item> q;
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> produced{0}, consumed{0};
  std::atomic<int> bad{0};

  std::vector<std::thread> threads;
  for (unsigned p = 0; p < P; ++p) {
    threads.emplace_back([&, p]() {
      std::uint64_t id = (static_cast<std::uint64_t>(p) + 1) << 40;
      for (std::uint64_t i = 0; i < per_producer; ++i) {
        Item it = make_item(++id);
        if (!it.p) {
          ++bad;
          continue;
        }
        {
          std::lock_guard<std::mutex> lk(m);
          q.push(it);
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (unsigned c = 0; c < C; ++c) {
    threads.emplace_back([&]() {
      for (;;) {
        Item it{nullptr, 0, 0};
        {
          std::lock_guard<std::mutex> lk(m);
          if (!q.empty()) {
            it = q.front();
            q.pop();
          }
        }
        if (it.p) {
          if (!check_item(it)) ++bad;
          hm_free(it.p);
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else if (done.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lk(m);
          if (q.empty()) break;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (unsigned p = 0; p < P; ++p) threads[p].join();
  done.store(true, std::memory_order_release);
  for (unsigned c = 0; c < C; ++c) threads[P + c].join();

  CHECK_EQ(bad.load(), 0);
  CHECK_EQ(produced.load(), consumed.load());
  CHECK_EQ(produced.load(), per_producer * P);
}

TEST(free_only_threads) {
  // The main thread allocates everything; worker threads only free. Workers
  // never allocate, so they never build a heap — every free is a remote push.
  const int N = 300000;
  std::vector<Item> items(N);
  for (int i = 0; i < N; ++i) items[i] = make_item(static_cast<std::uint64_t>(i) + 1);

  std::atomic<int> bad{0};
  std::atomic<int> next{0};
  const unsigned T = hw_threads();
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < T; ++t) {
    ts.emplace_back([&]() {
      for (;;) {
        int i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= N) break;
        if (!check_item(items[i])) ++bad;
        hm_free(items[i].p);
      }
    });
  }
  for (auto &th : ts) th.join();
  CHECK_EQ(bad.load(), 0);

  // The owner thread (this one) can still allocate afterward: collecting the
  // remote frees must not have corrupted any page.
  for (int i = 0; i < 1000; ++i) {
    void *p = hm_malloc(8 + (i % 2000));
    CHECK(p != nullptr);
    hm_free(p);
  }
}

int main() { return hm_test::run_all(); }
