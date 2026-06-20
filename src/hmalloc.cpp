/*
 * hmalloc — public C API (M3+).
 *
 * The libc stub is gone: small requests (<= SMALL_SIZE_MAX) go through the
 * per-thread heap (size classes, pages, segments); larger requests get their
 * own segment-aligned region from the OS layer, fronted by a tiny LargeHeader.
 * free() recovers which path a pointer came from with a single masked tag read
 * (kind_of), so it is O(1) and lock-free on the common path.
 */
#include "hmalloc/hmalloc.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

#include "central.h"
#include "heap.h"
#include "os.h"
#include "region_registry.h"
#include "segment.h"
#include "size_class.h"

using namespace hm;

namespace {

// Live large-allocation accounting (the large path is rare, so atomics are fine).
std::atomic<std::size_t> g_large_count{0};
std::atomic<std::size_t> g_large_bytes{0};

constexpr std::size_t round_up(std::size_t n, std::size_t a) {
  return (n + (a - 1)) & ~(a - 1);
}

// Serve a request larger than SMALL_SIZE_MAX (or one needing big alignment) from
// its own segment-aligned OS region. `align` is a power of two; the user pointer
// sits `offset` bytes into the region, just past the header, aligned as asked.
void *alloc_large(std::size_t size, std::size_t align) {
  std::size_t offset = LARGE_OFFSET;
  if (align > ALLOC_ALIGN) offset = round_up(LARGE_OFFSET, align);

  const std::size_t total = offset + size;
  if (total < size) return nullptr;  // overflow
  const std::size_t mmap_size = round_up(total, os_page_size());
  if (mmap_size < total) return nullptr;  // overflow on round-up

  void *mem = os_alloc_aligned(mmap_size, SEGMENT_SIZE);
  if (mem == nullptr) return nullptr;

  LargeHeader *h = new (mem) LargeHeader();
  h->kind = SegmentKind::Large;
  h->mmap_size = mmap_size;
  h->offset = offset;
  h->req_size = size;
  g_large_count.fetch_add(1, std::memory_order_relaxed);
  g_large_bytes.fetch_add(mmap_size, std::memory_order_relaxed);
  region_register(mem);
  // The user pointer stays within the first segment of the region, so masking
  // recovers this header even for multi-segment (huge) allocations.
  return static_cast<std::uint8_t *>(mem) + offset;
}

void free_large(void *p) {
  auto *h = static_cast<LargeHeader *>(region_base(p));
  const std::size_t msz = h->mmap_size;
  g_large_count.fetch_sub(1, std::memory_order_relaxed);
  g_large_bytes.fetch_sub(msz, std::memory_order_relaxed);
  region_unregister(h);
  os_free(h, msz);
}

}  // namespace

extern "C" {

void *hm_malloc(size_t size) {
  if (size == 0) return nullptr;
  if (size <= SMALL_SIZE_MAX) return heap_malloc(size);
  return alloc_large(size, ALLOC_ALIGN);
}

void hm_free(void *ptr) {
  if (ptr == nullptr) return;
  if (kind_of(ptr) == SegmentKind::Large)
    free_large(ptr);
  else
    heap_free(ptr);
}

void *hm_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) return nullptr;
  if (nmemb > SIZE_MAX / size) return nullptr;  // multiply overflow
  const std::size_t total = nmemb * size;

  if (total <= SMALL_SIZE_MAX) {
    void *p = heap_malloc(total);
    // Small blocks may be recycled and hold stale bytes; zero explicitly.
    if (p != nullptr) std::memset(p, 0, total);
    return p;
  }
  // Large regions are freshly mmap'd, which the kernel zero-fills for us.
  return alloc_large(total, ALLOC_ALIGN);
}

void *hm_realloc(void *ptr, size_t size) {
  if (ptr == nullptr) return hm_malloc(size);
  if (size == 0) {
    hm_free(ptr);
    return nullptr;
  }

  const std::size_t old_usable = hm_usable_size(ptr);
  // Keep the existing block when it already fits, unless we'd waste a lot of a
  // large allocation by holding it for a much smaller request.
  if (size <= old_usable) {
    if (old_usable <= SMALL_SIZE_MAX || size > old_usable / 2) return ptr;
  }

  void *np = hm_malloc(size);
  if (np == nullptr) return nullptr;
  std::memcpy(np, ptr, (size < old_usable) ? size : old_usable);
  hm_free(ptr);
  return np;
}

void *hm_aligned_alloc(size_t alignment, size_t size) {
  if (size == 0) return nullptr;
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
  if (alignment <= ALLOC_ALIGN) return hm_malloc(size);  // every block is 16-aligned

  // Prefer the small path: find a size class whose block size is a multiple of
  // `alignment` and >= size. Then every block in that class is alignment-aligned
  // and is a true block start, so free() needs no special handling.
  const std::size_t need = (size > alignment) ? size : alignment;
  if (need <= SMALL_SIZE_MAX) {
    std::uint32_t c = size_class(need);
    while (c < num_size_classes() && (class_to_size(c) % alignment) != 0) ++c;
    if (c < num_size_classes()) return heap_malloc(class_to_size(c));
  }

  // Otherwise use the large path, which aligns via the region offset. Alignments
  // beyond half a segment can't keep the user pointer in the first segment.
  if (alignment > SEGMENT_SIZE / 2) return nullptr;
  return alloc_large(size, alignment);
}

size_t hm_usable_size(void *ptr) {
  if (ptr == nullptr) return 0;
  if (kind_of(ptr) == SegmentKind::Large) {
    auto *h = static_cast<LargeHeader *>(region_base(ptr));
    return h->mmap_size - h->offset;
  }
  Segment *s = segment_of(ptr);
  return page_of(s, ptr)->block_size;
}

hm_stats_t hm_stats(void) {
  hm_stats_t out;
  std::memset(&out, 0, sizeof(out));

  const OsStats os = os_stats();
  out.bytes_reserved = os.bytes_mapped;
  out.peak_bytes_reserved = os.peak_mapped;

  const CentralStats c = central_stats();
  out.segments_mapped = c.segments;
  out.pages_in_use = c.pages_in_use;
  out.pages_free = c.free_pages;

  out.large_allocations = g_large_count.load(std::memory_order_relaxed);
  out.large_bytes = g_large_bytes.load(std::memory_order_relaxed);

  heap_global_counts(&out.malloc_count, &out.fast_path_count,
                     &out.slow_path_count);
  return out;
}

}  // extern "C"
