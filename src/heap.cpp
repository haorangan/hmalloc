/*
 * hmalloc — per-thread heap implementation (M4/M5). See heap.h.
 */
#include "heap.h"

#include <new>

#include "central.h"
#include "os.h"

namespace hm {

thread_local Heap *t_heap = nullptr;

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

}  // namespace

Heap *heap_create() {
  const std::size_t ps = os_page_size();
  const std::size_t bytes = (sizeof(Heap) + ps - 1) & ~(ps - 1);
  void *mem = os_alloc_aligned(bytes, ps);
  if (mem == nullptr) return nullptr;  // OOM: caller's malloc returns null
  Heap *h = new (mem) Heap();
  for (std::uint32_t c = 0; c < kNumSizeClasses; ++c) h->pages[c] = &g_empty_page;
  t_heap = h;
  return h;
}

void *heap_malloc_slow(Heap *h, std::uint32_t c) {
  if (h == nullptr) return nullptr;  // heap_create failed upstream

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

void heap_free_remote(Page *pg, void *p) {
  Block *b = static_cast<Block *>(p);
  Block *head = pg->thread_free.load(std::memory_order_relaxed);
  do {
    b->next = head;
  } while (!pg->thread_free.compare_exchange_weak(
      head, b, std::memory_order_release, std::memory_order_relaxed));
}

}  // namespace hm
