/*
 * hmalloc — central heap implementation (M3/M6). See central.h.
 */
#include "central.h"

#include <mutex>
#include <new>

#include "os.h"

namespace hm {
namespace {

struct CentralState {
  std::mutex mtx;
  Segment *segments = nullptr;     // all small segments (for stats / teardown)
  Page *free_pages = nullptr;      // pool of usable pages, linked via Page::next
  std::uint64_t free_page_count = 0;
  std::uint64_t num_segments = 0;
  std::uint64_t pages_in_use = 0;
};

CentralState &central() {
  static CentralState s;
  return s;
}

// Map a new small segment and push all its usable pages onto the free pool.
// Caller must hold the lock. Returns false on OOM.
bool map_segment(CentralState &g) {
  void *mem = os_alloc_aligned(SEGMENT_SIZE, SEGMENT_SIZE);
  if (mem == nullptr) return false;

  // Construct the header (zeroes the page metadata array, builds the atomics).
  Segment *s = new (mem) Segment();
  s->kind = SegmentKind::Small;
  s->page_count = static_cast<std::uint32_t>(PAGES_PER_SEGMENT);
  s->mmap_size = SEGMENT_SIZE;
  s->free_pages = 0;
  s->next = g.segments;
  g.segments = s;
  ++g.num_segments;

  // Page 0 holds this header; pages 1..N-1 are data areas available for reuse.
  auto *base = reinterpret_cast<std::uint8_t *>(s);
  for (std::size_t i = FIRST_USABLE_PAGE; i < PAGES_PER_SEGMENT; ++i) {
    Page *pg = &s->pages[i];
    pg->area = base + i * PAGE_SIZE;
    pg->in_use = false;
    pg->next = g.free_pages;
    g.free_pages = pg;
    ++g.free_page_count;
    ++s->free_pages;
  }
  return true;
}

}  // namespace

Page *central_acquire_page(Heap *owner, std::uint32_t cls,
                           std::uint32_t block_size) {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);

  if (g.free_pages == nullptr && !map_segment(g)) return nullptr;

  Page *pg = g.free_pages;
  g.free_pages = pg->next;
  --g.free_page_count;
  segment_of(pg->area)->free_pages--;
  ++g.pages_in_use;

  // Initialize for use by `owner`'s class `cls`. The page starts empty; blocks
  // are carved lazily from the bump area as the heap extends it.
  pg->free = nullptr;
  pg->used = 0;
  pg->capacity = static_cast<std::uint32_t>(PAGE_SIZE / block_size);
  pg->reserved = 0;
  pg->block_size = block_size;
  pg->size_class = cls;
  pg->in_use = true;
  pg->owner = owner;
  pg->next = nullptr;
  pg->prev = nullptr;
  pg->thread_free.store(nullptr, std::memory_order_relaxed);
  return pg;
}

void central_release_page(Page *pg) {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);

  pg->in_use = false;
  pg->owner = nullptr;
  pg->free = nullptr;
  pg->block_size = 0;
  pg->prev = nullptr;
  pg->next = g.free_pages;
  g.free_pages = pg;
  ++g.free_page_count;
  segment_of(pg->area)->free_pages++;
  --g.pages_in_use;
  // M6 will additionally unmap or cache segments whose pages are all free.
}

CentralStats central_stats() {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);
  return CentralStats{g.num_segments, g.pages_in_use, g.free_page_count};
}

}  // namespace hm
