//
// vmsmalloc Phase 3 — per-domain shared-state initialization.
//
// vmsmallocInit derives the set of CPU-bearing NUMA domains, allocates one
// per-domain shared buffer for each via VMSubstrate::reservePerDomainStaticBuffer
// (which places the pages on the requested domain and zero-fills them), and
// seeds MagazineTuning::currentK = kInitialK in every entry. The per-CPU
// CpuLocal pages are allocated/mapped/zeroed by VMSubstrate::createArena
// during the VMSubstrate init phase — vmsmallocInit relies on that contract
// and does no per-CPU work itself.
//
// Single-shot, single-threaded, BSP-only — asserts in debug builds.
//
// Registered in kernel/general.icd as [VMSubstrateSlab], depends_on VMSubstrate.
//

#include <stddef.h>
#include <core/mem.h>
#include <CpuLocal.h>
#include <kernel.h>
#include <kmemlayout.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>
#include "VMSubstrateSlab.h"

namespace kernel::mm::vmsmalloc {

// Storage for the per-domain buffer pointers declared in VMSubstrateSlab.h.
// Zero-initialized — a null slot means "domain `d` is not CPU-bearing".
void* perDomainBufs[kMaxDomains] = {};

bool vmsmallocInit() {
    static bool kInitialized = false;
    assert(!kInitialized, "vmsmallocInit called more than once");
    kInitialized = true;

    const auto& policy = kernel::numa::numaPolicy();

    // Derive the CPU-bearing-domain set D.  Iterating CPUs and marking
    // first-seen domains gives a deterministic ordering that lines up with
    // VMSubstrate::reservePerDomainStaticBuffer's NUMA-placement contract.
    bool domainHasCpu[kMaxDomains] = {};
    size_t cpuBearingDomainCount = 0;
    const size_t cpuCount = arch::processorCount();
    for (size_t i = 0; i < cpuCount; i++) {
        const numa::DomainID d = policy.homeDomain(static_cast<arch::ProcessorID>(i));
        assert(d.value < kMaxDomains,
               "vmsmallocInit: DomainID exceeds kMaxDomains");
        if (!domainHasCpu[d.value]) {
            domainHasCpu[d.value] = true;
            cpuBearingDomainCount++;
        }
    }

    // Reserve one per-domain buffer per CPU-bearing domain. The buffer is
    // zero-filled by reservePerDomainStaticBuffer; the partial-stack
    // instances and tuning counters are populated by vmsmallocLateInit
    // below (the tuning counters stay zero; the stacks are constructed with
    // maxChainLength = kInitialK and their head encodes the empty stack).
    for (size_t d = 0; d < kMaxDomains; d++) {
        if (!domainHasCpu[d]) continue;
        void* buf = VMSubstrate::reservePerDomainStaticBuffer(
            kPerDomainBufBytes, numa::DomainID{static_cast<uint16_t>(d)});
        perDomainBufs[d] = buf;
    }

    // Publish the VMSubstrate VA window and construct the per-(domain, class)
    // ChainedTreiberStack instances (Phase 5, P5-DEC-002 / P5-DEC-003).
    //
    // The window base is arena 0's base; its size is the span actually
    // covered by the live arenas (one per CPU plus the topmost static-buffer
    // slot). All arithmetic is derived from arch::pageTableDescriptor via
    // arenaVirtualBase / getKernelMemRegionSize — no hardcoded layout.
    const uintptr_t vmsBase = VMSubstrate::arenaVirtualBase(0).value;
    const size_t    vmsSize = (cpuCount + 1) * mm::getKernelMemRegionSize();
    vmsmallocLateInit(vmsBase, vmsSize);

    klog() << "VMSubstrateSlab init: perDomainBufs="
           << static_cast<uint64_t>(cpuBearingDomainCount)
           << ", perCpuCpuLocalPages="
           << static_cast<uint64_t>(cpuCount * kernel::kCpuLocalPages)
           << "\n";

    // Phase-5 boot smoke (P5-DEC-005): one allocation per class + whole-page
    // bypass, with alignment / magazine asserts. Leaks the allocations until
    // Phase 6 supplies vmsfree.
    vmsmallocBootSmoke();

    return true;
}

} // namespace kernel::mm::vmsmalloc
