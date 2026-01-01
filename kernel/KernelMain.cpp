//
// Created by Spencer Martin on 2/13/25.
//
#include <arch/hal/hal.h>
#include <kernel.h>
#include <core/Object.h>
#include <core/algo/GraphAlgorithms.h>
#include <arch/hal/interrupts.h>
#include <liballoc/InternalAllocatorDebug.h>
#include <timing.h>

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

    void timerTick() {
        static uint64_t ticks = 0;
        ticks += 10;
        if (ticks % 500 == 0) {
            klog << ticks << " ms elapsed\n";
        }
        if (ticks == 8000) {
            asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        }
    }

    extern "C" void kernel_main() {
        klog << "\n"; // newline to separate from the "Booting from ROM.." message from qemu

        klog << "Hello amd64 kernel world!\n";
        heapEarlyInit();
        //I am in agony - there is something fragile about the way things are linked
        //and calling presort_object_parent_lists() can apparently cause CRClass to break completely
        presort_object_parent_lists();
        run_global_constructors();
        klog << "Early data structure setup complete\n";

        hal::hwinit();

        klog << "Updating routing configuration\n";
        //FIXME this seems to take approximately 200 ms on my computer.
        //This seems oddly long...
        hal::interrupts::managed::updateRouting();
        klog << "Routing configuration updated\n";
        klog << "Total malloc usage " << LibAlloc::InternalAllocator::getAllocatorStats().totalBytesRequested << "\n";

        timing::getEventSource().registerCallback(timerTick);

        amd64::sti();

        auto delayInTicks = timing::getEventSource().calibrationData().nanosToTicks(10'000'000UL);
        timing::getEventSource().armPeriodic(delayInTicks);

        for (;;)
            asm volatile("hlt");
        //asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}