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

} // namespace kernel::interrupts

#endif // CROCOS_INTERRUPT_CONTEXT_DEPTHS_H
