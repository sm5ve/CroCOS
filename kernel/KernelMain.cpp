//
// Created by Spencer Martin on 2/13/25.
//
#include <arch/hal/hal.h>
#include <kernel.h>
#include <core/atomic.h>
#include <core/Object.h>
#include <core/GraphBuilder.h>
#include <core/algo/GraphAlgorithms.h>
#include <core/ds/Heap.h>

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

    extern "C" void kernel_main(){
        klog << "\n"; // newline to separate from the "Booting from ROM.." message from qemu

        klog << "Hello amd64 kernel world!\n";

        presort_object_parent_lists();
        run_global_constructors();

        hal::hwinit();

        klog << "init done!\n";

        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}