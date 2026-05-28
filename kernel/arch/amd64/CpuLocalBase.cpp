//
// AMD64 backend for arch::setCurrentCpuLocalBase / getCurrentCpuLocalBase
// (vmsmalloc Phase 4.5, P45-DEC-003).
//
// Uses wrgsbase / rdgsbase, which require CR4.FSGSBASE. CroCOS enables this
// bit at boot (QEMU `qemu64,+fsgsbase`). The pre-Phase-4.5 setLogicalProcessorID
// in kernel/arch/amd64/smp/smp.cpp also used wrgsbase, so the prerequisite
// is already met everywhere the kernel runs.
//
// Stray direct readers/writers of GSBase elsewhere in the tree are a code-
// review failure (grep audit during merge — see Phase 4.5 spec hazard).
//

#include <stdint.h>
#include <arch/CpuLocalBase.h>

namespace arch {

void setCurrentCpuLocalBase(void* ptr) noexcept {
    asm volatile("wrgsbase %0" : : "r"(reinterpret_cast<uint64_t>(ptr)));
}

void* getCurrentCpuLocalBase() noexcept {
    uint64_t v;
    asm volatile("rdgsbase %0" : "=r"(v));
    return reinterpret_cast<void*>(v);
}

} // namespace arch
