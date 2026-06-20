/*
 * hmalloc — drop-in malloc replacement (M8, optional).
 *
 * Built into libhmalloc_override.{so,dylib} when the CMake option
 * HMALLOC_OVERRIDE is ON. It routes the standard C allocation entry points and
 * the C++ operator new/delete family to hmalloc, so any program can use hmalloc
 * with no recompilation:
 *
 *   Linux:  LD_PRELOAD=./libhmalloc_override.so ./program
 *   macOS:  DYLD_INSERT_LIBRARIES=./libhmalloc_override.dylib \
 *           DYLD_FORCE_FLAT_NAMESPACE=1 ./program
 *
 * On ELF the standard names are defined as strong symbols (LD_PRELOAD wins). On
 * Mach-O, where you cannot simply redefine libc's malloc, we register __interpose
 * entries that the dynamic loader swaps in. operator new/delete are replaceable
 * by the standard, so we define them directly on both platforms.
 *
 * If the environment variable HMALLOC_OVERRIDE_REPORT is set, a library
 * destructor prints an hm_stats() summary to stderr at exit — handy to confirm
 * the override is actually in effect.
 */
#include "hmalloc/hmalloc.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <dlfcn.h> 
#include <errno.h>
#include <unistd.h>

#include "region_registry.h"
#include "segment.h"

namespace {

using hm::region_base;
using hm::region_owns;

// Look up the next definition of an allocation symbol (the real allocator), used
// for pointers hmalloc did not produce. Cached on first use.
template <typename Fn>
Fn real(const char *name) {
  return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

#if defined(__APPLE__)
constexpr const char *kFreeName = "free";
constexpr const char *kReallocName = "realloc";
constexpr const char *kUsableName = "malloc_size";
#else
constexpr const char *kFreeName = "free";
constexpr const char *kReallocName = "realloc";
constexpr const char *kUsableName = "malloc_usable_size";
#endif

// Is this a pointer hmalloc handed out? Decided from the address alone, so it
// never faults on a foreign pointer.
bool ours(void *p) { return region_owns(region_base(p)); }

void checked_free(void *p) {
  if (p == nullptr) return;
  if (ours(p)) {
    hm_free(p);
  } else {
    static auto rf = real<void (*)(void *)>(kFreeName);
    if (rf != nullptr) rf(p);
  }
}

void *checked_realloc(void *p, std::size_t size) {
  if (p == nullptr) return hm_malloc(size);
  if (ours(p)) return hm_realloc(p, size);
  static auto rr = real<void *(*)(void *, std::size_t)>(kReallocName);
  return (rr != nullptr) ? rr(p, size) : nullptr;
}

std::size_t checked_usable(void *p) {
  if (p == nullptr) return 0;
  if (ours(p)) return hm_usable_size(p);
  static auto ru = real<std::size_t (*)(const void *)>(kUsableName);
  return (ru != nullptr) ? ru(p) : 0;
}

void *aligned(std::size_t alignment, std::size_t size) {
  return hm_aligned_alloc(alignment, size);
}

int do_posix_memalign(void **out, std::size_t alignment, std::size_t size) {
  if (out == nullptr) return EINVAL;
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  void *p = hm_aligned_alloc(alignment, size);
  if (p == nullptr) return ENOMEM;
  *out = p;
  return 0;
}

void *do_valloc(std::size_t size) {
  long ps = ::sysconf(_SC_PAGESIZE);
  return hm_aligned_alloc(ps > 0 ? static_cast<std::size_t>(ps) : 4096, size);
}

}  // namespace

// ---------------------------------------------------------------------------
// C entry points
// ---------------------------------------------------------------------------
#if defined(__APPLE__)

// Mach-O interposition: map each libc entry point to our implementation.
#define HM_INTERPOSE(replacement, original)                                    \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *original;                                                      \
  } hm_interpose_##original __attribute__((section("__DATA,__interpose"))) = { \
      reinterpret_cast<const void *>(&replacement),                            \
      reinterpret_cast<const void *>(&original)}

extern "C" {
void *malloc(size_t);
void free(void *);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *aligned_alloc(size_t, size_t);
int posix_memalign(void **, size_t, size_t);
void *valloc(size_t);
size_t malloc_size(const void *);

void *hm_ov_malloc(size_t s) { return hm_malloc(s); }
void hm_ov_free(void *p) { checked_free(p); }
void *hm_ov_calloc(size_t n, size_t s) { return hm_calloc(n, s); }
void *hm_ov_realloc(void *p, size_t s) { return checked_realloc(p, s); }
void *hm_ov_aligned_alloc(size_t a, size_t s) { return aligned(a, s); }
int hm_ov_posix_memalign(void **o, size_t a, size_t s) {
  return do_posix_memalign(o, a, s);
}
void *hm_ov_valloc(size_t s) { return do_valloc(s); }
size_t hm_ov_malloc_size(const void *p) {
  return checked_usable(const_cast<void *>(p));
}
}

HM_INTERPOSE(hm_ov_malloc, malloc);
HM_INTERPOSE(hm_ov_free, free);
HM_INTERPOSE(hm_ov_calloc, calloc);
HM_INTERPOSE(hm_ov_realloc, realloc);
HM_INTERPOSE(hm_ov_aligned_alloc, aligned_alloc);
HM_INTERPOSE(hm_ov_posix_memalign, posix_memalign);
HM_INTERPOSE(hm_ov_valloc, valloc);
HM_INTERPOSE(hm_ov_malloc_size, malloc_size);

#else  // ELF: define the standard names directly.

extern "C" {
void *malloc(size_t s) { return hm_malloc(s); }
void free(void *p) { checked_free(p); }
void *calloc(size_t n, size_t s) { return hm_calloc(n, s); }
void *realloc(void *p, size_t s) { return checked_realloc(p, s); }
void *aligned_alloc(size_t a, size_t s) { return aligned(a, s); }
void *memalign(size_t a, size_t s) { return aligned(a, s); }
void *valloc(size_t s) { return do_valloc(s); }
int posix_memalign(void **o, size_t a, size_t s) {
  return do_posix_memalign(o, a, s);
}
size_t malloc_usable_size(void *p) { return checked_usable(p); }
}

#endif

// ---------------------------------------------------------------------------
// C++ operator new / delete (replaceable on every platform)
// ---------------------------------------------------------------------------
void *operator new(std::size_t s) {
  void *p = hm_malloc(s != 0 ? s : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void *operator new[](std::size_t s) { return ::operator new(s); }
void *operator new(std::size_t s, const std::nothrow_t &) noexcept {
  return hm_malloc(s != 0 ? s : 1);
}
void *operator new[](std::size_t s, const std::nothrow_t &) noexcept {
  return hm_malloc(s != 0 ? s : 1);
}
void operator delete(void *p) noexcept { checked_free(p); }
void operator delete[](void *p) noexcept { checked_free(p); }
void operator delete(void *p, std::size_t) noexcept { checked_free(p); }
void operator delete[](void *p, std::size_t) noexcept { checked_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { checked_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { checked_free(p); }

#if defined(__cpp_aligned_new)
void *operator new(std::size_t s, std::align_val_t a) {
  void *p = hm_aligned_alloc(static_cast<std::size_t>(a), s != 0 ? s : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void *operator new[](std::size_t s, std::align_val_t a) {
  return ::operator new(s, a);
}
void operator delete(void *p, std::align_val_t) noexcept { checked_free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { checked_free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  checked_free(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  checked_free(p);
}
#endif

// ---------------------------------------------------------------------------
// Optional exit-time report (set HMALLOC_OVERRIDE_REPORT to enable).
// ---------------------------------------------------------------------------
namespace {
__attribute__((destructor)) void hm_override_report() {
  if (std::getenv("HMALLOC_OVERRIDE_REPORT") == nullptr) return;
  hm_stats_t s = hm_stats();
  std::fprintf(stderr,
               "[hmalloc] bytes_reserved=%zu peak=%zu segments=%zu "
               "large=%zu malloc_count=%llu\n",
               s.bytes_reserved, s.peak_bytes_reserved, s.segments_mapped,
               s.large_allocations, s.malloc_count);
}
}  // namespace
