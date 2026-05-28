//
// Architecture-portable per-CPU base-register abstraction (vmsmalloc Phase 4.5,
// P45-DEC-003). Backend implementations in kernel/arch/<arch>/CpuLocalBase.cpp.
//
// AMD64 backend: wrgsbase / rdgsbase (requires CR4.FSGSBASE — CroCOS enables
// it; QEMU `qemu64,+fsgsbase`). ARMv8 backend would target TPIDR_EL1; RISC-V
// would target `tp`. Splitting the arch-specific accessor from the portable
// kernel::CpuLocal struct lets future ports add their backend without
// touching call sites.
//

#ifndef CROCOS_CPULOCALBASE_H
#define CROCOS_CPULOCALBASE_H

namespace arch {

// Sets the calling CPU's per-CPU base register to `ptr`.
//
// Called once per CPU during boot (BSP: at the end of VMSubstrate init by
// the BSP itself; APs: as the first instruction of their per-CPU bootstrap
// routine). Subsequent reads on the same CPU via getCurrentCpuLocalBase()
// return the stored pointer.
//
// Architecture cost: a single register-write instruction. No fence required
// because the value is only read on the same CPU.
void setCurrentCpuLocalBase(void* ptr) noexcept;

// Reads the calling CPU's per-CPU base register. Returns whatever pointer
// was last passed to setCurrentCpuLocalBase() on this CPU, or undefined
// content (typically zero) on a CPU whose base register has not yet been
// set. Always cheap (a single register read).
//
// kernel::cpuLocal() wraps this; prefer that accessor over a direct call
// here so the cpuLocalReady gate runs in debug builds.
void* getCurrentCpuLocalBase() noexcept;

} // namespace arch

#endif // CROCOS_CPU_LOCAL_BASE_H
