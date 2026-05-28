//
// vmsmalloc Phase 8 — test-only introspection into vmsmalloc internals.
//
// Declares accessors whose bodies live in vmsmalloc.cpp behind
// #ifdef CROCOS_VMSMALLOC_TEST_HARNESS (so they can read the file's
// anonymous-namespace state and the per-CPU magazines). Not compiled into the
// kernel build. Snapshots are only valid when the system is quiescent (between
// operations on a single thread, or after all worker threads have joined).
//

#ifndef CROCOS_VMSMALLOC_DEBUG_INTROSPECTION_H
#define CROCOS_VMSMALLOC_DEBUG_INTROSPECTION_H

#include <stddef.h>
#include <stdint.h>
#include <mem/NUMA.h>
#include <VMSubstrateSlab.h>

namespace kernel::mm::VMSubstrate::test {

// The calling thread's magazine for class c (its bound CPU's CpuLocal).
struct MagazineSnapshot { vmsmalloc::SlabDescriptorBase* head; uint32_t depth; };
MagazineSnapshot magazineSnapshot(size_t c);

// The per-domain shared stack for (d, c): the published top chain.
struct PartialStackSnapshot { vmsmalloc::SlabDescriptorBase* topHead; uint32_t topDepth; bool empty; };
PartialStackSnapshot partialStackSnapshot(numa::DomainID d, size_t c);

// Per-(domain, class) tuning counters + the stack's current maxChainLength.
struct TuningSnapshot { uint32_t maxChainLength; uint32_t overflowCount; uint32_t starvationCount; };
TuningSnapshot tuningSnapshot(numa::DomainID d, size_t c);

// Force-set maxChainLength to drive flush behavior in tests.
void setMaxChainLength(numa::DomainID d, size_t c, uint32_t k);

} // namespace kernel::mm::VMSubstrate::test

#endif // CROCOS_VMSMALLOC_DEBUG_INTROSPECTION_H
