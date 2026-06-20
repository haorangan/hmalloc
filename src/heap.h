/*
 * hmalloc — per-thread heap, fast path, and cross-thread frees (M4/M5).
 *
 * Each thread owns a Heap: one "active" page per size class (the fast-path head
 * of a per-class list of pages). The hot paths touch no lock and no atomic:
 *
 *   malloc: pop a block from the active page's local free list.
 *   free  : if this thread owns the block's page, push it onto that local list.
 *
 * Frees from another thread take the slow, lock-free path (heap_free_remote):
 * a single CAS onto the page's atomic `thread_free` list. The owner splices that
 * list back into its local free list in one atomic swap when it next runs dry
 * (page_collect, in heap.cpp). This is mimalloc's free-list sharding: the owner
 * never synchronizes, and remote frees contend only per-page, never globally.
 */
#ifndef HMALLOC_HEAP_H
#define HMALLOC_HEAP_H

#include <cstddef>
#include <cstdint>

#include "segment.h"
#include "size_class.h"

namespace hm {

struct Heap {
  // Active page per size class; the fast-path head of each class's page list.
  // A fresh heap points every entry at a shared empty sentinel whose free list
  // is always null, so the fast path falls straight through to the slow path.
  Page *pages[kNumSizeClasses];
};

// The current thread's heap pointer (null until this thread first allocates).
// Declared here so the inline hot paths can read it without a function call.
extern thread_local Heap *t_heap;

// Out-of-line helpers (defined in heap.cpp).
Heap *heap_create();                                  // make + install t_heap
void *heap_malloc_slow(Heap *h, std::uint32_t cls);   // refill + allocate
void heap_free_remote(Page *pg, void *p);             // CAS onto thread_free
void heap_on_page_empty(Page *pg);                    // page drained: maybe recycle

// Get this thread's heap, creating it on first use.
inline Heap *heap_get() {
  Heap *h = t_heap;
  return h ? h : heap_create();
}

// Allocate a small object (caller guarantees size <= SMALL_SIZE_MAX). The fast
// path is a free-list pop from the active page; everything else is slow.
inline void *heap_malloc(std::size_t size) {
  const std::uint32_t c = size_class(size);
  Heap *h = heap_get();
  Page *p = h->pages[c];
  Block *b = p->free;
  if (b != nullptr) {
    p->free = b->next;
    ++p->used;
    return b;
  }
  return heap_malloc_slow(h, c);
}

// Free a small-allocation pointer (caller guarantees it lives in a small page).
inline void heap_free(void *p) {
  Segment *s = segment_of(p);
  Page *pg = page_of(s, p);
  if (pg->owner.load(std::memory_order_relaxed) == t_heap) {  // local free
    Block *b = static_cast<Block *>(p);
    b->next = pg->free;
    pg->free = b;
    if (--pg->used == 0) heap_on_page_empty(pg);  // page idle: maybe recycle
  } else {  // remote free (also covers t_heap == nullptr: a free-only thread)
    heap_free_remote(pg, p);
  }
}

}  // namespace hm

#endif  // HMALLOC_HEAP_H
