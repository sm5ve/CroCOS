//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"
#include <kernel.h>
#include <kconfig.h>
#include <core/str.h>
#include <acpi.h>
#include <assert.h>
#include <timing/timing.h>
#include <core/math.h>
#include "multiboot.h"
#include <arch/amd64/interrupts/LegacyPIC.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/timers/HPET.h>
#include <kmemlayout.h>

extern uint32_t mboot_magic;
extern uint32_t mboot_table;

alignas(4096) uint64_t bootstrapPageDir[512];
extern volatile uint64_t boot_page_directory_pointer_table[512];

extern uint32_t phys_end;

size_t archProcessorCount;

namespace arch::amd64{

    void flushTLB(){
        asm volatile("mov %cr3, %rax\n"
                     "mov %rax, %cr3");
    }

    bool supportsFSGSBASE() {
        uint32_t eax, ebx, ecx, edx;
        eax = 0x07;
        ecx = 0;
        asm volatile("cpuid"
                : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "c"(ecx));
        return (ebx & (1 << 0)) != 0;
    }

    bool enableFSGSBase(){
        //TODO implement a fallback method
        //really I ought to just do this with a proper GDT or whatever... but this should be enough
        //to get the rudiments of SMP working.
        //temporaryHack(3, 3, 2026, "Implement a proper GDT");
        assert(supportsFSGSBASE(), "Your CPU doesn't support FSGSBASE");
        uint64_t cr4;
        asm volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 16);
        asm volatile ("mov %0, %%cr4" :: "r"(cr4));
        return true;
    }

    const uint64_t kernel_code_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 1ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed
    const uint64_t kernel_data_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 0ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed

    __attribute__((aligned(16)))
    static uint64_t gdt[] = {
            0x0000000000000000, // Null descriptor
            kernel_code_descriptor,
            kernel_data_descriptor
    };

    struct {
        uint16_t size;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
            .size = sizeof(gdt) - 1,
            .base = (uint64_t)&gdt
    };

    extern "C" void load_gdt(void*);

    bool initGDT() {
        load_gdt(&gdtr);
        return true;
    }

    bool searchForACPITables() {
        if (tryFindACPI() != ACPIDiscoveryResult::SUCCESS) {
            return false;
        }
        auto& madt = kernel::acpi::the<MADT>();
        archProcessorCount = madt.getEnabledProcessorCount();
        if (archProcessorCount == 0) {
            return false;
        }
        return true;
    }

    bool bspSetPID() {
        smp::setLogicalProcessorID(0);
        return true;
    }

    bool apSetPID() {
        auto lapicID = interrupts::getLAPICDomain() -> getID();
        auto pinfo = smp::getProcessorInfoForLapicID(static_cast<uint8_t>(lapicID));
        smp::setLogicalProcessorID(pinfo.logicalID);
        return true;
    }

    void temporaryPageFaultHandler(InterruptFrame& frame) {
        klog() << "Page fault at " << reinterpret_cast<void*>(frame.rip) << "\n";
        print_stacktrace(&frame.rbp);
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    }

    bool setupInterruptControllers() {
        auto& madt = kernel::acpi::the<acpi::MADT>();
        interrupts::disableLegacyPIC();
        interrupts::platform::setupCPUInterruptVectorFile(INTERRUPT_VECTOR_COUNT);
        interrupts::setupAPICs(madt);
        const auto exceptionVectors = make_shared<interrupts::ExceptionVectorDomain>(INTERRUPT_VECTOR_RESERVE_SIZE);
        interrupts::topology::registerDomain(exceptionVectors);
        const auto exceptionVectorConnector =
            make_shared<interrupts::platform::AffineConnector>(exceptionVectors,
            interrupts::platform::getCPUInterruptVectors(), INTERRUPT_VECTOR_RESERVE_START, 0, INTERRUPT_VECTOR_RESERVE_SIZE);
        interrupts::topology::registerExclusiveConnector(exceptionVectorConnector);

        using namespace interrupts::managed;
        registerHandler(InterruptSourceHandle(exceptionVectors, 14), temporaryPageFaultHandler);

        return true;
    }

    bool initPageTableAllocator() {
        assert(mboot_magic == 0x2BADB002, "Somehow the multiboot magic number is wrong. How did we get here?");
        mm::unmapIdentity();
        Vector<mm::PageAllocator::page_allocator_range_info> free_memory_regions;

        mboot_info* mbootInfo = early_boot_phys_to_virt(mm::phys_addr(mboot_table)).as_ptr<mboot_info>();
        mboot_mmap_entry* mbootMmapBase = early_boot_phys_to_virt(mm::phys_addr(mbootInfo -> mmap_ptr)).as_ptr<mboot_mmap_entry>();

        //Extract a list of free memory regions from the multiboot header, reserve the buffers requested by
        //the page allocator, and package that up to pass along to the page allocator
        //TODO make sure we double-check that the LAPIC/IOAPIC address ranges are completely reserved
        for(mboot_mmap_entry* mbootMmapEntry = mbootMmapBase;
            (uint64_t)mbootMmapEntry < (uint64_t)mbootMmapBase + mbootInfo -> mmap_len; mbootMmapEntry++){
            if(mbootMmapEntry -> type == 0x1){ //If the memory region is free, we'll add it to the list!
                mm::phys_memory_range range = {mm::phys_addr(mbootMmapEntry -> addr),
                                               mm::phys_addr(mbootMmapEntry -> addr + mbootMmapEntry -> len)};
                if(range.getSize() > (bigPageSize * 2)){
                    const auto buff = static_cast<uint64_t*>(mm::reservePageAllocatorBufferForRange(range));
                    free_memory_regions.push({range, buff});
                }
            }
        }

        mm::unmapTemporaryWindow();

        kernel::mm::PageAllocator::init(free_memory_regions, processorCount());
        //Find the memory range where the kernel resides and reserve it so we don't overwrite anything!
        mm::phys_memory_range range{.start=mm::phys_addr(nullptr), .end=mm::phys_addr(&phys_end)};
        mm::PageAllocator::reservePhysicalRange(range);
        return true;
    }

    bool initPageTableManager() {
        PageTableManager::init(arch::processorCount());
        return true;
    }
}