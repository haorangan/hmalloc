/*
 * hmalloc — per-thread heap implementation (M4/M5). See heap.h.
 */
#include "heap.h"

#include <mutex>
#include <new>

#include "central.h"
#include "os.h"

namespace hm {

thread_local Heap *t_heap = nullptr;

namespace {
// Registry of all live heaps, so hm_stats() can sum per-thread counters. Touched
// only on thread create/exit and by hm_stats(), never on the allocation path.
std::mutex g_registry_mtx;
Heap *g_registry = nullptr;
}  // namespace

void heap_global_counts(unsigned long long *malloc_count,
                        unsigned long long *fast, unsigned long long *slow) {
  unsigned long long m = 0, f = 0, s = 0;
  std::lock_guard<std::mutex> lk(g_registry_mtx);
  for (Heap *h = g_registry; h != nullptr; h = h->reg_next) {
    m += h->n_malloc.load(std::memory_order_relaxed);
    f += h->n_fast.load(std::memory_order_relaxed);
    s += h->n_slow.load(std::memory_order_relaxed);
  }
  *malloc_count = m;
  *fast = f;
  *slow = s;
}

namespace {

// Shared sentinel installed in every fresh heap slot. Its free list is null, so
// the fast path always misses and drops into heap_malloc_slow, which treats a
// slot still pointing here as "this class has no page yet". Never holds blocks.
Page g_empty_page{};

// Collect the page's cross-thread free list into its local free list in one
// atomic swap. Owner-thread only. `used` is decremented by the number of blocks
// reclaimed (remote frees left it counting them as still live; see heap.h).
void page_collect(Page *pg) {
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

// Carve a batch of fresh blocks from the page's bump area onto its free list.
// Done lazily (rather than threading the whole page when it is acquired) so a
// page that serves only a few objects never faults in its whole data area.
void page_extend(Page *pg) {
  const std::uint32_t remaining = pg->capacity - pg->reserved;
  if (remaining == 0) return;
  // Extend by roughly 8 KiB worth of blocks at a time (at least one).
  std::uint32_t batch = static_cast<std::uint32_t>(8192 / pg->block_size);
  if (batch == 0) batch = 1;
  if (batch > remaining) batch = remaining;

  std::uint8_t *start =
      pg->area + static_cast<std::size_t>(pg->reserved) * pg->block_size;
  Block *head = pg->free;  // normally null when we extend
  for (std::uint32_t i = batch; i-- > 0;) {
    Block *blk = reinterpret_cast<Block *>(
        start + static_cast<std::size_t>(i) * pg->block_size);
    blk->next = head;
    head = blk;
  }
  pg->free = head;
  pg->reserved += batch;
}

// Make `pg` able to satisfy an allocation if at all possible: reuse local free
// blocks, else collect remote frees, else carve more from the bump area.
bool page_make_available(Page *pg) {
  if (pg->free != nullptr) return true;
  page_collect(pg);
  if (pg->free != nullptr) return true;
  if (pg->reserved < pg->capacity) {
    page_extend(pg);
    return pg->free != nullptr;
  }
  return false;
}

Block *page_pop(Page *pg) {
  Block *b = pg->free;
  pg->free = b->next;
  ++pg->used;
  return b;
}

// Move page `p` to the front of class `c`'s list so the fast path finds it next.
void heap_rotate_to_front(Heap *h, std::uint32_t c, Page *p) {
  Page *head = h->pages[c];
  if (p == head) return;
  if (p->prev != nullptr) p->prev->next = p->next;
  if (p->next != nullptr) p->next->prev = p->prev;
  p->prev = nullptr;
  p->next = head;
  if (head != nullptr) head->prev = p;
  h->pages[c] = p;
}

// Bytes reserved from the OS for one Heap (page-rounded).
std::size_t heap_bytes() {
  const std::size_t ps = os_page_size();
  return (sizeof(Heap) + ps - 1) & ~(ps - 1);
}

// Hand this thread's heap back at thread exit. Each page is first collected (we
// are still the owner, so this is safe); fully-free pages are released to the
// central heap, pages with live objects are abandoned for central reclamation.
void heap_destroy(Heap *h) {
  for (std::uint32_t c = 0; c < kNumSizeClasses; ++c) {
    Page *p = h->pages[c];
    if (p == &g_empty_page) continue;
    while (p != nullptr && p != &g_empty_page) {
      Page *nx = p->next;
      page_collect(p);
      if (p->used == 0)
        central_release_page(p);
      else
        central_abandon_page(p);
      p = nx;
    }
    h->pages[c] = &g_empty_page;
  }
  {
    std::lock_guard<std::mutex> lk(g_registry_mtx);
    for (Heap **pp = &g_registry; *pp != nullptr; pp = &(*pp)->reg_next) {
      if (*pp == h) {
        *pp = h->reg_next;
        break;
      }
    }
  }
  os_free(h, heap_bytes());
}

// Destroys this thread's heap when the thread exits (thread_local destructor).
struct HeapGuard {
  bool armed = false;
  ~HeapGuard() {
    if (t_heap != nullptr) {
      heap_destroy(t_heap);
      t_heap = nullptr;
    }
  }
};
thread_local HeapGuard t_guard;

}  // namespace

Heap *heap_create() {
  void *mem = os_alloc_aligned(heap_bytes(), os_page_size());
  if (mem == nullptr) return nullptr;  // OOM: caller's malloc returns null
  Heap *h = new (mem) Heap();
  for (std::uint32_t c = 0; c < kNumSizeClasses; ++c) h->pages[c] = &g_empty_page;
  {
    std::lock_guard<std::mutex> lk(g_registry_mtx);
    h->reg_next = g_registry;
    g_registry = h;
  }
  t_heap = h;
  t_guard.armed = true;  // touch the guard so its thread-exit destructor runs
  return h;
}

void *heap_malloc_slow(Heap *h, std::uint32_t c) {
  if (h == nullptr) return nullptr;  // heap_create failed upstream
#ifdef HMALLOC_STATS
  h->n_slow.fetch_add(1, std::memory_order_relaxed);
#endif

  Page *head = h->pages[c];
  if (head != &g_empty_page) {
    // Walk the class's pages; use the first that can be made to serve a block,
    // rotating it to the front for the next allocation.
    for (Page *p = head; p != nullptr; p = p->next) {
      if (page_make_available(p)) {
        if (p != head) heap_rotate_to_front(h, c, p);
        return page_pop(p);
      }
    }
  }

  // No existing page can serve; acquire a fresh one and make it the active head.
  Page *np = central_acquire_page(h, c, class_to_size(c));
  if (np == nullptr) return nullptr;
  np->prev = nullptr;
  np->next = (head != &g_empty_page) ? head : nullptr;
  if (np->next != nullptr) np->next->prev = np;
  h->pages[c] = np;

  if (!page_make_available(np)) return nullptr;  // unreachable: fresh page
  return page_pop(np);
}

void heap_on_page_empty(Page *pg) {
  // The page has no live blocks. Keep it if it is the active head for its class
  // (so a balanced alloc/free workload doesn't churn pages with the central
  // heap); otherwise unlink it and return it for reuse by any class.
  Heap *h = pg->owner.load(std::memory_order_relaxed);
  const std::uint32_t c = pg->size_class;
  if (h->pages[c] == pg) return;

  pg->prev->next = pg->next;  // pg is not the head, so pg->prev != null
  if (pg->next != nullptr) pg->next->prev = pg->prev;
  central_release_page(pg);
}

void heap_free_remote(Page *pg, void *p) {
  Block *b = static_cast<Block *>(p);
  Block *head = pg->thread_free.load(std::memory_order_relaxed);
  do {
    b->next = head;
  } while (!pg->thread_free.compare_exchange_weak(
      head, b, std::memory_order_release, std::memory_order_relaxed));
}

}  // namespace hm
