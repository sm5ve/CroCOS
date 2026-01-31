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

    Core::PrintStream& emergencyLog() {
        return klogStream;
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

    using PagePool = mm::phys_addr[2048];

    PagePool page_pools[16];

    // Original naiveTest - page allocator stress test
    bool naiveTest() {
        klog() << "Running test on CPU " << arch::getCurrentProcessorID() << "\n";
        while (true) {
            for (size_t i = 0; i < sizeof(PagePool) / sizeof(mm::phys_addr); i++) {
                auto& page = page_pools[arch::getCurrentProcessorID()][i];
                page = mm::PageAllocator::allocateSmallPage();
                assert(page.value % arch::smallPageSize == 0, "The allocator has given me nonsense.");
            }
            for (size_t i = 0; i < sizeof(PagePool) / sizeof(mm::phys_addr); i++) {
                auto& page = page_pools[arch::getCurrentProcessorID()][i];
                assert(page.value % arch::smallPageSize == 0, "There is a strange memory corruption error.");
                mm::PageAllocator::freeSmallPage(page);
            }
        }
        return true;
    }

    [[noreturn]] bool spin() {
        for (;;)
            asm volatile("hlt");
    }

    bool enqueueShutdown() {
        arch::amd64::sti();
        klog() << "Enqueuing shutdown\n";
        timing::enqueueEvent([] {
            klog() << "\nGoodbye :)\n";
            asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        }, 2000);
        return true;
    }

    extern "C" void kernel_main() {
        klog() << "\n"; // newline to separate from the "Booting from ROM.." message from qemu
        init::kinit(true, KERNEL_INIT_LOG_LEVEL, false);

        for (;;)
            asm volatile("hlt");
        //asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}