/*
 * hmalloc — a modern, high-performance memory allocator.
 *
 * Public C API. The implementation is C++ but the surface is C-compatible so
 * that hmalloc can act as a drop-in malloc replacement.
 */
#ifndef HMALLOC_H
#define HMALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes, suitably aligned for any built-in type.
 * Returns NULL on failure (or for size 0, a unique freeable pointer is allowed
 * but not required; hmalloc returns NULL for size 0). */
void *hm_malloc(size_t size);

/* Free a pointer previously returned by hm_malloc/calloc/realloc/aligned_alloc.
 * Freeing NULL is a no-op. */
void hm_free(void *ptr);

/* Allocate `nmemb * size` bytes, zero-initialized. Detects multiply overflow. */
void *hm_calloc(size_t nmemb, size_t size);

/* Resize `ptr` to `size` bytes, preserving min(old, new) bytes of contents.
 * realloc(NULL, size) == malloc(size); realloc(ptr, 0) frees and returns NULL. */
void *hm_realloc(void *ptr, size_t size);

/* Allocate `size` bytes aligned to `alignment` (a power of two). */
void *hm_aligned_alloc(size_t alignment, size_t size);

/* Number of usable bytes in the allocation `ptr` points at (>= requested). */
size_t hm_usable_size(void *ptr);

/* Live allocator statistics. The memory/structure fields are always populated;
 * the call-count fields are tracked only when hmalloc is built with
 * -DHMALLOC_STATS (otherwise they read 0), so the hot path stays free of
 * counters in the default build. Snapshots are intended to be read at a quiet
 * point; counters may lag slightly under concurrent allocation. */
typedef struct hm_stats_t {
  size_t bytes_reserved;       /* bytes currently mapped from the OS */
  size_t peak_bytes_reserved;  /* high-water mark of bytes_reserved */
  size_t segments_mapped;      /* small segments currently mapped */
  size_t pages_in_use;         /* small pages owned by a live thread heap */
  size_t pages_free;           /* small pages pooled for reuse */
  size_t large_allocations;    /* live large (> small threshold) allocations */
  size_t large_bytes;          /* bytes mapped for live large allocations */
  unsigned long long malloc_count;     /* hm_malloc calls (small path) */
  unsigned long long fast_path_count;  /* served from a thread-local free list */
  unsigned long long slow_path_count;  /* needed refill/collect/new page */
} hm_stats_t;

/* Snapshot the live statistics above. */
hm_stats_t hm_stats(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HMALLOC_H */
