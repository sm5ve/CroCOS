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

    // Reserve one per-domain buffer per CPU-bearing domain.
    for (size_t d = 0; d < kMaxDomains; d++) {
        if (!domainHasCpu[d]) continue;
        void* buf = VMSubstrate::reservePerDomainStaticBuffer(
            kPerDomainBufBytes, numa::DomainID{static_cast<uint16_t>(d)});
        perDomainBufs[d] = buf;

        // Seed MagazineTuning::currentK on every entry. Other tuning
        // fields stay zero per the reservePerDomainStaticBuffer zero-fill
        // contract; TreiberHead.head stays zero, which decodes to the
        // empty-stack tagged head per DEC-015.
        auto* tuning = tuningFor(numa::DomainID{static_cast<uint16_t>(d)});
        for (size_t c = 0; c < kNumSizeClasses; c++) {
            tuning[c].currentK.store(kInitialK, RELEASE);
        }
    }

    klog() << "VMSubstrateSlab init: perDomainBufs="
           << static_cast<uint64_t>(cpuBearingDomainCount)
           << ", perCpuCpuLocalPages="
           << static_cast<uint64_t>(cpuCount * kernel::kCpuLocalPages)
           << "\n";

    return true;
}

} // namespace kernel::mm::vmsmalloc
