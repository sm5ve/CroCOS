//
// BSP bootstrap CpuLocal (vmsmalloc Phase 4.5).
//
// Zero-initialized in BSS — logicalID=0 (matches BSP_LOGICAL_ID),
// interruptDepths and magazines all zero. bspSetPID points GSBase at this
// struct early in the processor_early phase; from that point on,
// kernel::cpuLocal() works on the BSP. VMSubstrate's init re-points the
// BSP's GSBase to its arena-resident CpuLocal (also zero-initialized,
// also with logicalID=0) once that page exists.
//
// APs do not use this struct — each AP's ap_routine points its own GSBase
// at the arena-resident CpuLocal for its logical ID.
//

#include <CpuLocal.h>
#include <mem/VMSubstrate.h>

namespace kernel {

CpuLocal bspBootstrapCpuLocal{};

void initBspCpuLocal() noexcept {
    arch::setCurrentCpuLocalBase(&bspBootstrapCpuLocal);
}

void initApCpuLocal(arch::ProcessorID id) noexcept {
    arch::setCurrentCpuLocalBase(kernel::mm::VMSubstrate::cpuLocalPageFor(id));
}

} // namespace kernel
