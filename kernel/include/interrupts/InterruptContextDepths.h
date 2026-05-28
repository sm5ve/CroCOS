//
// InterruptContextDepths — per-CPU counters for nested interrupt contexts
// (vmsmalloc Phase 4.5 ships the struct only; Phase 7 adds the
// InterruptKind enum, kindForVector mapping, and InterruptContextGuard RAII
// type to this same header).
//
// Each counter is decremented and incremented by Phase 7's
// InterruptContextGuard around dispatchInterrupt. Phase 7's entry-point
// asserts on vmsmalloc / vmsfree consult these counters to enforce the
// forbidden-context invariant (DEC-014: vmsmalloc must not run from IRQ,
// NMI, #UD, #DF, #GP, #MC; #PF is conditionally legal per the amendment).
//
// The struct is a field of kernel::CpuLocal. Cache-line aligned so the
// surrounding CpuLocal fields don't false-share with the depth counters
// under contended access patterns.
//

#ifndef CROCOS_INTERRUPT_CONTEXT_DEPTHS_H
#define CROCOS_INTERRUPT_CONTEXT_DEPTHS_H

#include <stdint.h>
#include <arch.h>   // arch::InterruptKind (portable interrupt classification)

namespace kernel::interrupts {

// arch::cacheLineSize is not exposed as a kernel-wide constant; hardcoded
// to 64 per CLAUDE.md (matches Phase 3's kVmsmallocCacheLine usage).
inline constexpr unsigned int kInterruptContextDepthsAlignment = 64;

struct alignas(kInterruptContextDepthsAlignment) InterruptContextDepths {
    uint32_t irq;   // external IRQ (vector >= 32)
    uint32_t nmi;   // vector 2
    uint32_t ud;    // vector 6  (#UD undefined opcode)
    uint32_t df;    // vector 8  (#DF double fault)
    uint32_t gp;    // vector 13 (#GP general protection)
    uint32_t mc;    // vector 18 (#MC machine check)
    uint32_t pf;    // vector 14 (#PF page fault)
    // Phase 7 may add more fields here as InterruptKind grows; cache-line
    // alignment provides headroom.
};

static_assert(alignof(InterruptContextDepths) == kInterruptContextDepthsAlignment);

// ─── Phase 7 — interrupt-context tracking consumer logic ───────────────────
//
// The RAII guard that maintains the per-CPU depth counters around
// dispatchInterrupt, keyed by the portable arch::InterruptKind. The
// vector→kind classification itself is architecture-specific and lives behind
// arch::interruptKind (see arch.h); this layer only tracks depths per kind.
// The guard's bodies and the currentCpuInterruptDepths accessor live in
// InterruptContextDepths.cpp (they touch kernel::cpuLocal(), whose header
// includes THIS one — defining them out-of-line keeps the include graph
// acyclic).

// RAII guard constructed at the top of dispatchInterrupt: the constructor
// increments the matching per-CPU depth counter, the destructor decrements it.
// arch::InterruptKind::Other is a no-op (no counter). CPU-local, no atomics —
// the dispatch path runs with interrupts disabled.
class InterruptContextGuard {
public:
    explicit InterruptContextGuard(arch::InterruptKind k) noexcept;
    ~InterruptContextGuard() noexcept;
    InterruptContextGuard(const InterruptContextGuard&) = delete;
    InterruptContextGuard& operator=(const InterruptContextGuard&) = delete;
private:
    arch::InterruptKind kind_;
};

// Read-only view of the calling CPU's depth counters (defined in the .cpp,
// where kernel::cpuLocal() is reachable).
const InterruptContextDepths& currentCpuInterruptDepths() noexcept;

} // namespace kernel::interrupts

#endif // CROCOS_INTERRUPT_CONTEXT_DEPTHS_H
