/*
 * hmalloc — OS memory layer implementation (M1). See os.h.
 */
#include "os.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

#if defined(MAP_ANONYMOUS)
#define HM_MAP_ANON MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define HM_MAP_ANON MAP_ANON
#else
#error "no anonymous mmap flag on this platform"
#endif

namespace hm {
namespace {

// Cached OS page size. Computed once on first use; sysconf is not free.
std::size_t cached_page_size() {
  static const std::size_t ps = []() -> std::size_t {
    long v = ::sysconf(_SC_PAGESIZE);
    return (v > 0) ? static_cast<std::size_t>(v) : 4096;
  }();
  return ps;
}

// --- accounting -----------------------------------------------------------
std::atomic<std::size_t> g_bytes_mapped{0};
std::atomic<std::size_t> g_peak_mapped{0};
std::atomic<std::uint64_t> g_map_calls{0};
std::atomic<std::uint64_t> g_unmap_calls{0};

void account_map(std::size_t n) {
  std::size_t now = g_bytes_mapped.fetch_add(n, std::memory_order_relaxed) + n;
  // Bump the peak monotonically. Racy readers may briefly see a slightly stale
  // peak, which is fine for a statistic.
  std::size_t prev = g_peak_mapped.load(std::memory_order_relaxed);
  while (now > prev &&
         !g_peak_mapped.compare_exchange_weak(prev, now,
                                              std::memory_order_relaxed)) {
  }
  g_map_calls.fetch_add(1, std::memory_order_relaxed);
}

void account_unmap(std::size_t n) {
  g_bytes_mapped.fetch_sub(n, std::memory_order_relaxed);
  g_unmap_calls.fetch_add(1, std::memory_order_relaxed);
}

void *raw_mmap(std::size_t size) {
  void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | HM_MAP_ANON, -1, 0);
  return (p == MAP_FAILED) ? nullptr : p;
}

constexpr bool is_pow2(std::size_t x) { return x != 0 && (x & (x - 1)) == 0; }

}  // namespace

std::size_t os_page_size() { return cached_page_size(); }

void *os_alloc_aligned(std::size_t size, std::size_t alignment) {
  const std::size_t ps = cached_page_size();
  if (size == 0 || alignment == 0) return nullptr;
  if (!is_pow2(alignment)) return nullptr;
  if (size % ps != 0 || alignment % ps != 0) return nullptr;

  // Fast path: a page-granular mmap is already at least page aligned, so if the
  // caller only needs page alignment we can map exactly `size` with no slop.
  if (alignment <= ps) {
    void *p = raw_mmap(size);
    if (p) account_map(size);
    return p;
  }

  // General path: over-map by one alignment, then trim the head and tail slop
  // so we return exactly `size` bytes at an `alignment`-aligned address and
  // hold no excess address space.
  //
  //   base            aligned                 aligned+size      base+over
  //    |--- head -->|--------- size ---------|<---- tail ----------|
  const std::size_t over = size + alignment;
  auto base = reinterpret_cast<std::uintptr_t>(raw_mmap(over));
  if (base == 0) return nullptr;

  const std::uintptr_t aligned = (base + (alignment - 1)) & ~(alignment - 1);
  const std::size_t head = aligned - base;
  const std::size_t tail = over - head - size;

  if (head != 0) ::munmap(reinterpret_cast<void *>(base), head);
  if (tail != 0) ::munmap(reinterpret_cast<void *>(aligned + size), tail);

  // We mapped `over`, immediately returned `head + tail`, net `size` retained.
  account_map(over);
  account_unmap(head + tail);
  return reinterpret_cast<void *>(aligned);
}

void os_free(void *ptr, std::size_t size) {
  if (ptr == nullptr || size == 0) return;
  ::munmap(ptr, size);
  account_unmap(size);
}

bool os_purge(void *ptr, std::size_t size) {
  if (ptr == nullptr || size == 0) return false;
#if defined(MADV_FREE)
  // MADV_FREE lets the kernel reclaim lazily and is cheaper; pages re-fault to
  // zero only if actually reclaimed before reuse.
  return ::madvise(ptr, size, MADV_FREE) == 0;
#elif defined(MADV_DONTNEED)
  return ::madvise(ptr, size, MADV_DONTNEED) == 0;
#else
  (void)ptr;
  (void)size;
  return false;
#endif
}

OsStats os_stats() {
  return OsStats{g_bytes_mapped.load(std::memory_order_relaxed),
                 g_peak_mapped.load(std::memory_order_relaxed),
                 g_map_calls.load(std::memory_order_relaxed),
                 g_unmap_calls.load(std::memory_order_relaxed)};
}

}  // namespace hm
