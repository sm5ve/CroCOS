//
// vmsmalloc Phase 8 — kernel/arch environment stubs for the harness.
//
// Provides userspace bodies for the kernel/arch symbols vmsmalloc.cpp pulls in
// transitively: the per-CPU base register (thread_local), the logical-CPU-ID
// accessor (per-thread bound CPU), klog/emergencyLog/panic plumbing, the mock
// NUMA policy, and the all-zeros interrupt-context depths (no real interrupts
// in userspace, so vmsmalloc's DEC-014 forbidden-context check never fires —
// by design, per the Phase-8 spec).
//

#include <arch.h>
#include <kernel.h>
#include <panic.h>
#include <mem/NUMA.h>                          // mock
#include <mem/VMSubstrate.h>                   // mock (cpuLocalPageFor)
#include <CpuLocal.h>                          // real
#include <interrupts/InterruptContextDepths.h> // real
#include "MockCpuLocal.h"

// ─── arch: per-CPU base register + logical ID (thread_local) ───────────────
namespace arch {
namespace {
    thread_local void*       tlCpuLocalBase = nullptr;
    thread_local ProcessorID tlBoundCpu     = 0;
    size_t                   gProcessorCount = 8;
}

void  setCurrentCpuLocalBase(void* ptr) noexcept { tlCpuLocalBase = ptr; }
void* getCurrentCpuLocalBase() noexcept          { return tlCpuLocalBase; }

ProcessorID getCurrentProcessorID() { return tlBoundCpu; }
size_t      processorCount()        { return gProcessorCount; }
size_t      getCacheLineSize()      { return 64; }

namespace test { void setBoundCpu(ProcessorID i) { tlBoundCpu = i; } }
}

// ─── kernel: logging + panic plumbing ──────────────────────────────────────
namespace kernel {
namespace {
    class NullStream : public Core::PrintStream {
    protected:
        void putString(const char*) override {}
    };
    NullStream gNullStream;
}

Core::AtomicPrintStream klog()        { return Core::AtomicPrintStream(gNullStream); }
Core::PrintStream&      emergencyLog() { return gNullStream; }
void print_stacktrace()           {}
void print_stacktrace(uintptr_t*) {}
}

// ─── kernel::numa: configurable mock policy ────────────────────────────────
namespace kernel::numa {
namespace { NUMAPolicy gPolicy; }
const NUMAPolicy& numaPolicy() { return gPolicy; }
namespace test {
    void configure(size_t cpus, size_t domains, NUMAPolicy::Mapping mapping) {
        gPolicy.configure(cpus, domains, mapping);
    }
}
}

// ─── kernel::interrupts: all-zeros depths (never in interrupt context) ──────
namespace kernel::interrupts {
namespace { thread_local InterruptContextDepths tlZeroDepths{}; }
const InterruptContextDepths& currentCpuInterruptDepths() noexcept { return tlZeroDepths; }
}

// ─── kernel::test: per-thread CpuLocal binding ─────────────────────────────
namespace arch { namespace test { void setBoundCpu(ProcessorID); } }
namespace kernel::test {
void bindThreadToCpu(arch::ProcessorID i) {
    void* page = kernel::mm::VMSubstrate::cpuLocalPageFor(i);
    auto* cl = static_cast<kernel::CpuLocal*>(page);
    cl->logicalID = i;
    arch::setCurrentCpuLocalBase(page);
    arch::test::setBoundCpu(i);
}
}
