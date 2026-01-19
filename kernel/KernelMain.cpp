//
// Created by Spencer Martin on 2/13/25.
//
#include <kernel.h>
#include <core/Object.h>
#include <core/algo/GraphAlgorithms.h>
#include <interrupts/interrupts.h>
#include <liballoc/InternalAllocatorDebug.h>
#include <timing/timing.h>
#include <arch/amd64/smp.h>
#include <init.h>
#include <arch.h>

extern "C" void (*__init_array_start[])(void) __attribute__((weak));
extern "C" void (*__init_array_end[])(void) __attribute__((weak));

namespace kernel{
    arch::SerialPrintStream EarlyBootStream;
    Core::PrintStream& klogStream = EarlyBootStream;

    Core::AtomicPrintStream klog() {
        return Core::AtomicPrintStream(klogStream);
    }

    bool runGlobalConstructors(){
        for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ctor++) {
            (*ctor)();
        }
        return true;
    }

    bool initCRClassMetadata() {
        presort_object_parent_lists();
        return true;
    }

    extern "C" void kernel_main() {
        klog() << "\n"; // newline to separate from the "Booting from ROM.." message from qemu
        init::kinit(true, KERNEL_INIT_LOG_LEVEL, false);

        timing::enqueueEvent([] {
            klog() << "\nGoodbye :)\n";
            asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        }, 1000);

        for (;;)
            asm volatile("hlt");
        //asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}