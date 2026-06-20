/*
 * hmalloc implementation.
 *
 * M0 STATUS: this is a thin, correct stub that delegates to the system
 * allocator. It exists so the public API, tests, and benchmark harness compile
 * and run end-to-end from day one. Starting at M1 the internals below are
 * replaced milestone by milestone with the real size-class / segment /
 * per-thread-heap allocator described in DESIGN.md. The API never changes.
 */
#include "hmalloc/hmalloc.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <malloc/malloc.h> /* malloc_size */
#elif defined(__linux__)
#include <malloc.h> /* malloc_usable_size */
#endif

extern "C" {

void *hm_malloc(size_t size) {
  if (size == 0) return nullptr;
  return std::malloc(size);
}

void hm_free(void *ptr) { std::free(ptr); }

void *hm_calloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) return nullptr;
  /* Overflow check: nmemb * size must not wrap. */
  if (nmemb > SIZE_MAX / size) return nullptr;
  return std::calloc(nmemb, size);
}

void *hm_realloc(void *ptr, size_t size) {
  if (size == 0) {
    std::free(ptr);
    return nullptr;
  }
  return std::realloc(ptr, size);
}

void *hm_aligned_alloc(size_t alignment, size_t size) {
  if (size == 0) return nullptr;
  /* alignment must be a power of two and a multiple of sizeof(void*). */
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  void *p = nullptr;
  if (posix_memalign(&p, alignment, size) != 0) return nullptr;
  return p;
}

size_t hm_usable_size(void *ptr) {
  if (ptr == nullptr) return 0;
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(__linux__)
  return malloc_usable_size(ptr);
#else
  return 0;
#endif
}

} /* extern "C" */
