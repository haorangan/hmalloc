/*
 * hmalloc — region ownership registry implementation. See region_registry.h.
 *
 * A mutex-guarded open-addressing hash set of region base addresses. The table
 * is backed directly by the OS layer (never by hmalloc itself), so registration
 * triggered from inside the central heap cannot recurse back into the allocator.
 */
#include "region_registry.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "constants.h"
#include "os.h"

namespace hm {
namespace {

constexpr std::uintptr_t EMPTY = 0;
constexpr std::uintptr_t TOMB = static_cast<std::uintptr_t>(-1);
// Region bases are non-zero and SEGMENT_SIZE-aligned, so they never collide with
// the EMPTY/TOMB sentinels.

struct Registry {
  std::mutex mtx;
  std::uintptr_t *slots = nullptr;
  std::size_t cap = 0;     // power of two
  std::size_t size = 0;    // live entries
  std::size_t tombs = 0;   // tombstones
};

Registry &reg() {
  static Registry r;
  return r;
}

std::uintptr_t *alloc_slots(std::size_t cap) {
  const std::size_t bytes = cap * sizeof(std::uintptr_t);
  const std::size_t ps = os_page_size();
  void *m = os_alloc_aligned((bytes + ps - 1) & ~(ps - 1), ps);
  return static_cast<std::uintptr_t *>(m);  // zero-filled by the OS == all EMPTY
}

std::size_t index_of(std::uintptr_t cap_mask, std::uintptr_t key) {
  // Mix the segment-index bits; bases are SEGMENT_SIZE-aligned so the low
  // SEGMENT_SHIFT bits are zero and carry no information.
  std::uintptr_t h = key >> SEGMENT_SHIFT;
  h *= 0x9e3779b97f4a7c15ull;
  return static_cast<std::size_t>(h & cap_mask);
}

// Insert into a table assumed to have room and no tombstones (used by rehash).
void raw_insert(std::uintptr_t *slots, std::size_t cap, std::uintptr_t key) {
  std::size_t i = index_of(cap - 1, key);
  while (slots[i] != EMPTY) i = (i + 1) & (cap - 1);
  slots[i] = key;
}

void grow_locked(Registry &r, std::size_t new_cap) {
  std::uintptr_t *old = r.slots;
  const std::size_t old_cap = r.cap;
  std::uintptr_t *fresh = alloc_slots(new_cap);
  if (fresh == nullptr) return;  // out of memory: keep the old table
  for (std::size_t i = 0; i < old_cap; ++i)
    if (old[i] != EMPTY && old[i] != TOMB) raw_insert(fresh, new_cap, old[i]);
  r.slots = fresh;
  r.cap = new_cap;
  r.tombs = 0;
  if (old != nullptr) os_free(old, old_cap * sizeof(std::uintptr_t));
}

}  // namespace

void region_register(void *base) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lk(r.mtx);
  if (r.slots == nullptr) grow_locked(r, 1024);
  // Rehash/grow when the load factor (including tombstones) passes ~0.7. Grow
  // capacity only when live entries are dense; otherwise rehash same-size to
  // clear tombstones.
  if ((r.size + r.tombs + 1) * 10 >= r.cap * 7)
    grow_locked(r, (r.size * 2 >= r.cap) ? r.cap * 2 : r.cap);
  if (r.slots == nullptr) return;

  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(base);
  std::size_t i = index_of(r.cap - 1, key);
  std::size_t first_tomb = SIZE_MAX;
  while (r.slots[i] != EMPTY) {
    if (r.slots[i] == key) return;  // already present
    if (r.slots[i] == TOMB && first_tomb == SIZE_MAX) first_tomb = i;
    i = (i + 1) & (r.cap - 1);
  }
  if (first_tomb != SIZE_MAX) {
    r.slots[first_tomb] = key;
    --r.tombs;
  } else {
    r.slots[i] = key;
  }
  ++r.size;
}

void region_unregister(void *base) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lk(r.mtx);
  if (r.slots == nullptr) return;
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(base);
  std::size_t i = index_of(r.cap - 1, key);
  while (r.slots[i] != EMPTY) {
    if (r.slots[i] == key) {
      r.slots[i] = TOMB;
      --r.size;
      ++r.tombs;
      return;
    }
    i = (i + 1) & (r.cap - 1);
  }
}

bool region_owns(void *base) {
  Registry &r = reg();
  std::lock_guard<std::mutex> lk(r.mtx);
  if (r.slots == nullptr) return false;
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(base);
  std::size_t i = index_of(r.cap - 1, key);
  while (r.slots[i] != EMPTY) {
    if (r.slots[i] == key) return true;
    i = (i + 1) & (r.cap - 1);
  }
  return false;
}

}  // namespace hm
