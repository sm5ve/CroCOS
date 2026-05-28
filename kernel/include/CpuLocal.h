//
// kernel::CpuLocal — per-CPU kernel state (vmsmalloc Phase 4.5, P45-DEC-001).
//
// One CpuLocal struct lives on each CPU's arena-resident metadata page (the
// reservation introduced by Phase 3, sized by kCpuLocalBytes / kCpuLocalPages
// below). It is accessed via kernel::cpuLocal(), which reads the CPU's
// per-CPU base register (GSBase on AMD64) under the cpuLocalReady gate.
//
// What lives in CpuLocal:
//   - logicalID: the CPU's logical processor ID. Replaces the pre-Phase-4.5
//     "GSBase low 8 bits = ProcessorID" scheme. Written once during
//     VMSubstrate::createArena(i); immutable thereafter.
//   - interruptDepths: nested-interrupt depth counters consumed by Phase 7's
//     InterruptContextGuard / vmsmalloc entry-point asserts. Field type only
//     in Phase 4.5; consumer logic in Phase 7.
//   - magazines: per-CPU magazine head + depth per vmsmalloc size class
//     (DEC-034). Populated and walked by Phase 5/6 vmsmalloc/vmsfree.
//
// Future scheduler fields (preempt_count, current_thread, etc.) land here.
//

#ifndef CROCOS_CPULOCAL_H
#define CROCOS_CPULOCAL_H

#include <stddef.h>
#include <arch.h>
#include <arch/CpuLocalBase.h>
#include <core/math.h>
#include <interrupts/InterruptContextDepths.h>
// VMSubstrateSlab.h pulls in kernel::mm::vmsmalloc::Magazine and
// kNumSizeClasses. It's the implementation-internal vmsmalloc header per
// Phase 2 (DEC-028), but Phase 4.5's P45-DEC-001 folds the magazine array
// directly into CpuLocal, so the magazine type must be visible here.
// Compiled with kernel/mm on the include path (see kernel/CMakeLists.txt).
#include <VMSubstrateSlab.h>

namespace kernel {

// 64-byte cache-line alignment (arch::cacheLineSize is not exposed as a
// kernel-wide constant; documented value per CLAUDE.md).
inline constexpr size_t kCpuLocalAlignment = 64;

struct alignas(kCpuLocalAlignment) CpuLocal {
    arch::ProcessorID logicalID;
    // implicit padding to next cache line — InterruptContextDepths is
    // alignas(64).
    kernel::interrupts::InterruptContextDepths interruptDepths;
    kernel::mm::vmsmalloc::Magazine
        magazines[kernel::mm::vmsmalloc::kNumSizeClasses];
};

// Page-sized region consumed by VMSubstrate.cpp's arena layout math. Sized
// to round sizeof(CpuLocal) up to a whole number of pages. With the current
// envelope (one cache line of logicalID + one cache line of
// InterruptContextDepths + kNumSizeClasses (8) cache lines of magazines =
// ~640 B), this is one 4 KiB page on AMD64 with room for growth.
inline constexpr size_t kCpuLocalBytes =
    divideAndRoundUp(sizeof(CpuLocal), arch::smallPageSize) * arch::smallPageSize;
inline constexpr size_t kCpuLocalPages = kCpuLocalBytes / arch::smallPageSize;
static_assert(kCpuLocalBytes % arch::smallPageSize == 0,
              "kCpuLocalBytes must be a whole number of pages");
static_assert(kCpuLocalPages >= 1,
              "kCpuLocalPages must reserve at least one page");

// BSP-only bootstrap CpuLocal in BSS, zero-initialized — logicalID=0 by
// construction (matches BSP_LOGICAL_ID), interruptDepths and magazines zero.
// bspSetPID points GSBase at this struct very early in processor_early
// phase, so kernel::cpuLocal() is valid from that point on. VMSubstrate's
// init re-points the BSP's GSBase to its arena-resident CpuLocal once that
// page exists; the BSS-bootstrap struct becomes unused thereafter (its
// magazine state is never populated because vmsmalloc runs post-init).
//
// The earlier P45-DEC-002 cpuLocalReady gate was removed when the BSS
// bootstrap landed — GSBase being structurally valid from bspSetPID
// onward replaces the assertion-based migration discipline.
extern CpuLocal bspBootstrapCpuLocal;

// The typed per-CPU accessor. GSBase (or the arch equivalent) is pointed
// at a valid CpuLocal struct from very early boot (initBspCpuLocal for
// the BSP, initApCpuLocal for each AP).
inline CpuLocal& cpuLocal() noexcept {
    return *static_cast<CpuLocal*>(arch::getCurrentCpuLocalBase());
}

// Architecture-agnostic logical-processor-ID accessor. Used by
// arch::getCurrentProcessorID and any other caller that needs the current
// CPU's ID without touching arch-specific facilities.
inline arch::ProcessorID getLogicalProcessorID() noexcept {
    return cpuLocal().logicalID;
}

// ─── BSP / AP per-CPU bootstrap (arch-agnostic) ────────────────────────────
//
// Each architecture's processor_early init routine calls these. The BSP
// bootstrap routine calls initBspCpuLocal once during BSP-side setup; the
// AP routine calls initApCpuLocal once on each AP after the AP determined
// its own logical ID (lookup is arch-specific — e.g. on AMD64 from the
// LAPIC ID — but the resulting setCurrentCpuLocalBase call is not). Body
// defined in CpuLocal.cpp.
//
// After initBspCpuLocal, GSBase points at bspBootstrapCpuLocal until
// VMSubstrate's init re-points it at the arena-resident CpuLocal page.
// After initApCpuLocal, GSBase on the AP points directly at the
// arena-resident CpuLocal page for that AP (allocated by createArena
// during BSP-side VMSubstrate init, which has already completed by the
// time any AP runs its routine).
void initBspCpuLocal() noexcept;
void initApCpuLocal(arch::ProcessorID id) noexcept;

} // namespace kernel

#endif // CROCOS_CPULOCAL_H
