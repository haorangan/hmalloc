/*
 * hmalloc — central heap implementation (M3/M6). See central.h.
 *
 * Pages are pooled per segment (Segment::free_list) rather than in one global
 * list, so the central heap can tell in O(1) when a whole segment has gone idle
 * and hand it back to the OS. Segments with at least one free page are kept in a
 * doubly-linked "available" list for O(1) page acquisition. One fully-free
 * segment is cached to absorb churn; additional idle segments are unmapped.
 *
 * Thread exit: a heap's empty pages are released here directly; its pages that
 * still hold live objects are *abandoned* (owner cleared) onto a global list.
 * Other threads keep freeing those objects onto each page's atomic thread_free
 * list; a sweep (run lazily, just before we would otherwise map a new segment)
 * collects them and reclaims any page that has become fully free.
 */
#include "central.h"

#include <mutex>
#include <new>

#include "os.h"
#include "region_registry.h"

namespace hm {
namespace {

// Keep at most this many fully-free segments mapped; unmap the rest so RSS
// follows the live set down after a spike.
constexpr std::uint64_t SEGMENT_CACHE = 1;

// Non-null owner stamped on an abandoned page. It must differ from every real
// heap pointer AND from nullptr: a thread that has never allocated has a null
// heap, so a null abandoned-owner would collide and be mistaken for a local
// free (owner == t_heap). A distinct sentinel keeps such frees on the remote
// path without adding a branch to the allocator's hot free path.
char g_abandon_tag;
Heap *const ABANDONED_OWNER = reinterpret_cast<Heap *>(&g_abandon_tag);

struct CentralState {
  std::mutex mtx;
  Segment *segments = nullptr;    // all small segments (Segment::next)
  Segment *avail = nullptr;       // segments with >= 1 free page (avail_*)
  Page *abandoned = nullptr;      // pages from exited threads (Page::next)
  std::uint64_t num_segments = 0;
  std::uint64_t free_segments = 0;   // segments that are entirely free
  std::uint64_t free_page_count = 0;
  std::uint64_t pages_in_use = 0;    // pages owned by a live heap
  std::uint64_t abandoned_count = 0;
};

// Constructed once, never destroyed: thread-exit heap destructors call in here
// during program shutdown, so the central state (and its mutex) must outlive all
// thread-local objects rather than race their destruction order.
CentralState &central() {
  alignas(CentralState) static unsigned char storage[sizeof(CentralState)];
  static CentralState *s = new (storage) CentralState();
  return *s;
}

void avail_push(CentralState &g, Segment *s) {
  s->avail_prev = nullptr;
  s->avail_next = g.avail;
  if (g.avail != nullptr) g.avail->avail_prev = s;
  g.avail = s;
  s->in_avail = true;
}

void avail_remove(CentralState &g, Segment *s) {
  if (s->avail_prev != nullptr)
    s->avail_prev->avail_next = s->avail_next;
  else
    g.avail = s->avail_next;
  if (s->avail_next != nullptr) s->avail_next->avail_prev = s->avail_prev;
  s->avail_prev = s->avail_next = nullptr;
  s->in_avail = false;
}

void all_remove(CentralState &g, Segment *s) {
  if (g.segments == s) {
    g.segments = s->next;
    return;
  }
  for (Segment *p = g.segments; p != nullptr; p = p->next) {
    if (p->next == s) {
      p->next = s->next;
      return;
    }
  }
}

// Return a page to its segment's free pool. Does not touch pages_in_use /
// abandoned_count (callers own that accounting). May unmap an idle segment.
void pool_return(CentralState &g, Page *pg) {
  Segment *s = segment_of(pg->area);
  pg->in_use = false;
  pg->owner.store(nullptr, std::memory_order_relaxed);
  pg->free = nullptr;
  pg->block_size = 0;
  pg->prev = nullptr;

  const bool was_empty_segment = (s->free_count == 0);
  pg->next = s->free_list;
  s->free_list = pg;
  ++s->free_count;
  ++g.free_page_count;
  if (was_empty_segment) avail_push(g, s);

  if (s->free_count == USABLE_PAGES_PER_SEGMENT) {
    ++g.free_segments;
    if (g.free_segments > SEGMENT_CACHE) {
      avail_remove(g, s);
      all_remove(g, s);
      --g.num_segments;
      --g.free_segments;
      g.free_page_count -= USABLE_PAGES_PER_SEGMENT;
      region_unregister(s);
      os_free(s, s->mmap_size);  // s is invalid after this
    }
  }
}

// Collect an abandoned page's cross-thread frees into its local free list. No
// owner exists, so this runs under the central lock. Returns blocks reclaimed.
void collect_locked(Page *pg) {
  Block *list = pg->thread_free.exchange(nullptr, std::memory_order_acquire);
  if (list == nullptr) return;
  std::uint32_t n = 1;
  Block *tail = list;
  while (tail->next != nullptr) {
    tail = tail->next;
    ++n;
  }
  tail->next = pg->free;
  pg->free = list;
  pg->used -= n;
}

// Sweep abandoned pages: collect their remaining frees and reclaim any that have
// become fully free. Caller holds the lock.
void sweep_abandoned(CentralState &g) {
  Page **pp = &g.abandoned;
  while (*pp != nullptr) {
    Page *pg = *pp;
    collect_locked(pg);
    if (pg->used == 0) {
      *pp = pg->next;  // unlink before pool_return reuses pg->next
      --g.abandoned_count;
      pool_return(g, pg);
    } else {
      pp = &pg->next;
    }
  }
}

bool map_segment(CentralState &g) {
  void *mem = os_alloc_aligned(SEGMENT_SIZE, SEGMENT_SIZE);
  if (mem == nullptr) return false;

  Segment *s = new (mem) Segment();
  s->kind = SegmentKind::Small;
  s->page_count = static_cast<std::uint32_t>(PAGES_PER_SEGMENT);
  s->mmap_size = SEGMENT_SIZE;
  s->free_list = nullptr;
  s->free_count = 0;

  auto *base = reinterpret_cast<std::uint8_t *>(s);
  for (std::size_t i = FIRST_USABLE_PAGE; i < PAGES_PER_SEGMENT; ++i) {
    Page *pg = &s->pages[i];
    pg->area = base + i * PAGE_SIZE;
    pg->in_use = false;
    pg->next = s->free_list;
    s->free_list = pg;
    ++s->free_count;
  }
  g.free_page_count += s->free_count;

  s->next = g.segments;
  g.segments = s;
  ++g.num_segments;
  ++g.free_segments;
  avail_push(g, s);
  region_register(s);
  return true;
}

}  // namespace

Page *central_acquire_page(Heap *owner, std::uint32_t cls,
                           std::uint32_t block_size) {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);

  if (g.avail == nullptr) {
    sweep_abandoned(g);  // try to reclaim before growing the footprint
    if (g.avail == nullptr && !map_segment(g)) return nullptr;
  }

  Segment *s = g.avail;
  if (s->free_count == USABLE_PAGES_PER_SEGMENT) --g.free_segments;
  Page *pg = s->free_list;
  s->free_list = pg->next;
  --s->free_count;
  --g.free_page_count;
  if (s->free_count == 0) avail_remove(g, s);
  ++g.pages_in_use;

  pg->free = nullptr;
  pg->used = 0;
  pg->capacity = static_cast<std::uint32_t>(PAGE_SIZE / block_size);
  pg->reserved = 0;
  pg->block_size = block_size;
  pg->size_class = cls;
  pg->in_use = true;
  pg->owner.store(owner, std::memory_order_relaxed);
  pg->next = nullptr;
  pg->prev = nullptr;
  pg->thread_free.store(nullptr, std::memory_order_relaxed);
  return pg;
}

void central_release_page(Page *pg) {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);
  --g.pages_in_use;
  pool_return(g, pg);
}

void central_abandon_page(Page *pg) {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);
  pg->owner.store(ABANDONED_OWNER, std::memory_order_relaxed);
  pg->prev = nullptr;
  pg->next = g.abandoned;
  g.abandoned = pg;
  ++g.abandoned_count;
  --g.pages_in_use;
}

CentralStats central_stats() {
  CentralState &g = central();
  std::lock_guard<std::mutex> lk(g.mtx);
  return CentralStats{g.num_segments, g.pages_in_use, g.free_page_count};
}

}  // namespace hm
