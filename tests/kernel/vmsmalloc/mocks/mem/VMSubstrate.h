//
// vmsmalloc Phase 8 — mock <mem/VMSubstrate.h> for the userspace harness.
//
// Shadows the real kernel header via include-path ordering. Declares the same
// VMSubstrate API surface vmsmalloc.cpp links against, but the page-allocation
// primitives are backed by an mmap region (see MockVMSubstrate.cpp) rather than
// kernel page tables. ensureTLBEntryFresh is a no-op (no TLB in userspace).
//
// kWindowAddressBits is the literal 39 (one AMD64 PML4 entry / 512 GiB), so
// vmsmalloc's DEC-015 encoding uses the same 27-bit offset / 37-bit counter
// split as production (P8-DEC-007). The mmap region is far smaller, so the
// upper offset bits stay zero — identical math, no test-only code path.
//
// make<T> / destroy<T> / SafePtr mirror the real header so the (deferred)
// MakeStaticAssertsTest can exercise them; the integration tests call
// vmsmalloc / vmsfree directly.
//

#ifndef CROCOS_MOCK_VMSUBSTRATE_H
#define CROCOS_MOCK_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/SafePtr.h>   // the shared definition; this file supplies only ensureTLBEntryFresh
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <core/utility.h>      // forward, placement new
#include <VMSubstrateSlab.h>   // sizeClassFor / slotAlignment / kNumSizeClasses

namespace kernel::mm::VMSubstrate {

    // VA-window width: literal 39 bits in the harness (P8-DEC-007). vmsmalloc's
    // VmsHeadEncoding derives kOffsetBits = 39 - log2(pageSize) = 27 from this.
    inline constexpr unsigned kWindowAddressBits = 39;

    // Page primitives — mmap-backed (MockVMSubstrate.cpp).
    void* allocPage();
    // DEC-048: the failable sibling. Null on pool exhaustion or under the
    // harness's scripted-failure hook (test::setPageAllocFailAt).
    void* tryAllocPage();
    void  freePage(void* p);
    // DEC-047 slab-reclaim sibling of freePage. The harness does not simulate
    // the read-only sentinel remap (no page tables in userspace); it recycles
    // the page like freePage. See MockVMSubstrate.cpp.
    void  reclaimSlabPage(void* p);
    void* mapMMIOPage(phys_addr paddr);

    // No-op in userspace: no TLB, no dirty bitmap. The call site in
    // vmsmalloc.cpp is still exercised (source-order correctness).
    // `noexcept` matches the shared declaration in <mem/SafePtr.h>. It briefly did
    // not: this file was the only one of the three substrates that had it, and
    // nothing could catch that while each declared the function privately. Sharing
    // the declaration turned the drift into a compile error on the first build.
    inline bool ensureTLBEntryFresh(void*) CROCOS_FRESHNESS_NOEXCEPT { return false; }  // userspace: never stale

    // Phase-3 storage primitives.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    // vmsmalloc DEC-050/051: the failable, runtime-callable variant. The mock
    // fails on window exhaustion and under the harness's scripted-failure hook,
    // so the RCU domain-lifecycle unwind paths are drivable here.
    void* tryReservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);
    virt_addr arenaVirtualBase(size_t index);

    // Convention-internal (defined by vmsmalloc.cpp, compiled into the harness).
    void* vmsmalloc(size_t size);
    // DEC-048: same path, null returns in place of the two exhaustion panics.
    void* vmsmallocTry(size_t size);
    void  vmsfree(void*);

    // SafePtr is shared with the kernel and the other mock — <mem/SafePtr.h>,
    // included above. This file keeps only `ensureTLBEntryFresh`, which is the
    // one thing a mock substrate legitimately defines differently.

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::make<T>: sizeof(T) exceeds a page");
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                      "size class. Pad T into a power-of-two size class, split the "
                      "over-aligned subobject, or use a different allocator.");
        auto* mem = vmsmalloc(sizeof(T));
        // malloc (not calloc) semantics — mirror the real header: default-init
        // with no args so trivial T is left uninitialized, not zero-filled.
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;
        } else {
            return new (mem) T(forward<Ts>(args)...);
        }
    }

    // DEC-048's failable entry point, mirroring the real header.
    template <typename T, typename... Ts>
    SafePtr<T> tryMake(Ts&&... args) {
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::tryMake<T>: sizeof(T) exceeds a page");
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::tryMake<T>: alignof(T) exceeds the slot alignment of its "
                      "size class.");
        void* mem = vmsmallocTry(sizeof(T));
        if (!mem) return SafePtr<T>(nullptr);
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;
        } else {
            return new (mem) T(forward<Ts>(args)...);
        }
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) { obj->~T(); vmsfree(obj.raw()); }
    }

    // ─── Harness control surface (not in the real header) ───
    namespace test {
        // mmap the VA region, carve per-domain + per-CPU storage, construct the
        // per-(domain,class) ChainedTreiberStacks (via vmsmallocLateInit).
        void initialize(size_t cpuCount, size_t domainCount);
        void shutdown();              // munmap + reset all state
        size_t activePageCount();     // live slab/whole-page pages (leak check)
        // Scripted failure for tryReservePerDomainStaticBuffer: fail every call
        // from the n'th onward (-1 disables). Lets a consumer's creation-unwind
        // path be driven without first exhausting a 64 MiB arena.
        void   setStaticReservationFailAt(long n);
        // Same idea for tryAllocPage (DEC-048): fail every call from the n'th
        // onward (-1 disables). Drives vmsmallocTry's two exhaustion sites.
        void   setPageAllocFailAt(long n);
    }
}

#endif // CROCOS_MOCK_VMSUBSTRATE_H
