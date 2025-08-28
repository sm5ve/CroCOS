//
// Created by Spencer Martin on 2/13/25.
//
#include <arch/hal/hal.h>
#include <kernel.h>
#include <core/Object.h>
#include <core/algo/GraphAlgorithms.h>
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <liballoc/InternalAllocatorDebug.h>

extern "C" void (*__init_array_start[])(void) __attribute__((weak));
extern "C" void (*__init_array_end[])(void) __attribute__((weak));

extern void testGraph();

namespace kernel{
    hal::SerialPrintStream EarlyBootStream;
    Core::PrintStream& klog = EarlyBootStream;

    void run_global_constructors(){
        for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ctor++) {
            (*ctor)();
        }
    }

    extern "C" void kernel_main() {
        klog << "\n"; // newline to separate from the "Booting from ROM.." message from qemu

        klog << "Hello amd64 kernel world!\n";
        heapEarlyInit();
        presort_object_parent_lists();
        run_global_constructors();
        klog << "Early data structure setup complete\n";

        klog << "Coarse allocator is using " <<
                           LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator()
                           << " bytes\n";

        hal::hwinit();

        klog << "Coarse allocator is using " <<
                   LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator()
                   << " bytes\n";

        klog << "init done!\n";

        auto interruptTopologyGraph = hal::interrupts::topology::getTopologyGraph();
        assert(interruptTopologyGraph.occupied(), "Interrupt topology graph must be initialized");
        //klog << interruptTopologyGraph -> getVertexCount();
        algorithm::graph::printAsDOT(klog, *interruptTopologyGraph);
        for (auto vertex : interruptTopologyGraph -> vertices()) {
            auto domain = interruptTopologyGraph -> getVertexLabel(vertex);
            klog << "Considering domain:\n";
            if (auto receiver = crocos_dynamic_cast<hal::interrupts::platform::InterruptReceiver>(domain)) {
                klog << "Domain is receiver and has " << receiver -> getReceiverCount() << " receivers\n";
            }
            else if (auto emitter = crocos_dynamic_cast<hal::interrupts::platform::InterruptEmitter>(domain)) {
                klog << "Domain is pure emitter and has " << emitter -> getEmitterCount() << " emitters\n";
            }
        }

        klog << "Coarse allocator is using " <<
                    LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator()
                    << " bytes\n";

        auto builder = hal::interrupts::managed::createRoutingGraphBuilder();

        klog << "Interrupt routing graph builder made\n";
        klog << "has " << builder -> getVertices().getSize() << " vertices\n";
        klog << "has " << builder -> getCurrentEdgeCount() << " edges\n";

        klog << "Coarse allocator is using " <<
            LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator()
            << " bytes\n";

        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}