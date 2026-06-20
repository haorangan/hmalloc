/*
 * hmalloc — region ownership registry.
 *
 * Records the segment-aligned base address of every region hmalloc maps (small
 * segments and large allocations). Its sole purpose is the drop-in malloc
 * override (override.cpp): when a program is fully interposed, free() may be
 * handed pointers that some other allocator produced (e.g. allocations made
 * during process startup before interposition took effect). The override must
 * route those to the real free, and ours to hm_free — but it cannot inspect a
 * foreign pointer's "segment header" to decide, because masking to a 4 MiB
 * boundary and reading there can fault on memory we never mapped.
 *
 * region_owns() answers "did hmalloc map this region?" by hash-set lookup on the
 * base address alone, never dereferencing the pointer, so it is fault-safe.
 *
 * Registration happens only when segments/large regions are mapped or unmapped
 * (off the allocation hot path), so the cost is negligible and the core fast
 * path is unaffected.
 */
#ifndef HMALLOC_REGION_REGISTRY_H
#define HMALLOC_REGION_REGISTRY_H

namespace hm {

void region_register(void *base);    // base must be SEGMENT_SIZE-aligned
void region_unregister(void *base);
bool region_owns(void *base);        // is `base` a currently-registered region?

}  // namespace hm

#endif  // HMALLOC_REGION_REGISTRY_H
