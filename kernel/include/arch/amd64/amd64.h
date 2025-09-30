//
// Created by Spencer Martin on 2/12/25.
//

#ifndef CROCOS_AMD64_TABLES_H
#define CROCOS_AMD64_TABLES_H

#include "kconfig.h"
#include "kernel.h"
#include <core/ds/Vector.h>
#include "FlushPlanner.h"

namespace kernel::amd64 {
    //Derived from https://wiki.osdev.org/CPUID, or tables 3-19 and 3-20 in volume 2 of the Intel manual.
    enum class CPUID_FEAT_LEAF_BITMAP{
        ECX_SSE3         = 1 << 0,
        ECX_PCLMUL       = 1 << 1,
        ECX_DTES64       = 1 << 2,
        ECX_MONITOR      = 1 << 3,
        ECX_DS_CPL       = 1 << 4,
        ECX_VMX          = 1 << 5,
        ECX_SMX          = 1 << 6,
        ECX_EST          = 1 << 7,
        ECX_TM2          = 1 << 8,
        ECX_SSSE3        = 1 << 9,
        ECX_CID          = 1 << 10,
        ECX_SDBG         = 1 << 11,
        ECX_FMA          = 1 << 12,
        ECX_CX16         = 1 << 13,
        ECX_XTPR         = 1 << 14,
        ECX_PDCM         = 1 << 15,
        ECX_PCID         = 1 << 17,
        ECX_DCA          = 1 << 18,
        ECX_SSE4_1       = 1 << 19,
        ECX_SSE4_2       = 1 << 20,
        ECX_X2APIC       = 1 << 21,
        ECX_MOVBE        = 1 << 22,
        ECX_POPCNT       = 1 << 23,
        ECX_TSC          = 1 << 24,
        ECX_AES          = 1 << 25,
        ECX_XSAVE        = 1 << 26,
        ECX_OSXSAVE      = 1 << 27,
        ECX_AVX          = 1 << 28,
        ECX_F16C         = 1 << 29,
        ECX_RDRAND       = 1 << 30,
        ECX_HYPERVISOR   = 1 << 31,

        EDX_FPU          = 1 << 0,
        EDX_VME          = 1 << 1,
        EDX_DE           = 1 << 2,
        EDX_PSE          = 1 << 3,
        EDX_TSC          = 1 << 4,
        EDX_MSR          = 1 << 5,
        EDX_PAE          = 1 << 6,
        EDX_MCE          = 1 << 7,
        EDX_CX8          = 1 << 8,
        EDX_APIC         = 1 << 9,
        EDX_SEP          = 1 << 11,
        EDX_MTRR         = 1 << 12,
        EDX_PGE          = 1 << 13,
        EDX_MCA          = 1 << 14,
        EDX_CMOV         = 1 << 15,
        EDX_PAT          = 1 << 16,
        EDX_PSE36        = 1 << 17,
        EDX_PSN          = 1 << 18,
        EDX_CLFLUSH      = 1 << 19,
        EDX_DS           = 1 << 21,
        EDX_ACPI         = 1 << 22,
        EDX_MMX          = 1 << 23,
        EDX_FXSR         = 1 << 24,
        EDX_SSE          = 1 << 25,
        EDX_SSE2         = 1 << 26,
        EDX_SS           = 1 << 27,
        EDX_HTT          = 1 << 28,
        EDX_TM           = 1 << 29,
        EDX_IA64         = 1 << 30,
        EDX_PBE          = 1 << 31
    };

    constexpr size_t INTERRUPT_VECTOR_COUNT = 256;
    constexpr size_t INTERRUPT_VECTOR_RESERVE_START = 0;
    constexpr size_t INTERRUPT_VECTOR_RESERVE_SIZE = 32;

    using ProcessorID = uint8_t;

    //Wrapper for the CPUID instruction. The first four parameters are references to uint32_t's corresponding to
    //the registers EAX-EDX. The last parameter is the value to load into EAX (the "leaf" per the
    // Intel manual) before calling CPUID.
    void cpuid(uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx, uint32_t leaf);

    //Simple wrapper for the outb instruction to print out a string on the serial port
    void serialOutputString(const char* str);

    //Intel recommends that reentrant locks should occupy an entire cache line, which is usually 64 bytes wide.
    //At least, that's what the osdev wiki says (https://wiki.osdev.org/Spinlock)

    void outb(uint16_t port, uint8_t out);
    uint8_t inb(uint16_t port);
    void outw(uint16_t port, uint16_t word);
    uint16_t inw(uint16_t port);
    void cli();
    void sti();
    void invlpg(uint64_t addr);

    void wrmsr(uint32_t msr, uint64_t value);
    uint64_t rdmsr(uint32_t msr);

    void hwinit();

    inline void mfence() {
        asm volatile("mfence" ::: "memory");
    }

    inline constexpr mm::virt_addr early_boot_phys_to_virt(mm::phys_addr x){
        return mm::virt_addr(x.value + VMEM_OFFSET);
    }

    inline constexpr mm::phys_addr early_boot_virt_to_phys(mm::virt_addr x){
        return mm::phys_addr(x.value - VMEM_OFFSET);
    }

    namespace PageTableManager{
        struct CompositeHandle{
            uint64_t value;
        };

        struct PartialHandle{
            uint64_t value;
        };

        const uint64_t partialPageStructureAlignment = (1ul << 39);
        const uint32_t pcidMax = (1 << 12);

        void init(size_t processorCount);

        PartialHandle makePartialPageStructure(mm::virt_addr base, size_t size);
        CompositeHandle makeCompositePageStructure(uint32_t pcid);
        void destroyPartialPageStructure(PartialHandle);
        void destroyCompositePageStructure(CompositeHandle);

        [[nodiscard]]
        mm::phys_addr resolveVirtualAddress(PartialHandle, mm::virt_addr);
        [[nodiscard]]
        mm::phys_addr resolveVirtualAddress(CompositeHandle, mm::virt_addr);

        //Only VirtualAddressZones should be modifying their mappings, so we only expose methods to modify mappings
        //using the PartialHandle
        void mapAddress(PartialHandle, mm::phys_addr, mm::virt_addr, mm::PageSize,
                        mm::PageMappingPermissions permissions,
                        mm::PageMappingCacheType cacheType = mm::PageMappingCacheType::FULLY_CACHED);
        void unmapAddress(PartialHandle, mm::virt_addr);

        //Only supports mapping pages of the same size. For mixed-size mappings, call this function multiple times
        //on the relevant subsets. In particular, the 0th entry in the vector will be mapped at the base address,
        //then the 1st at (base address + page size), and so on
        void mapAddresses(PartialHandle, Vector<mm::phys_addr>&, mm::virt_addr base, mm::PageSize,
                          mm::PageMappingPermissions permissions,
                          mm::PageMappingCacheType cacheType = mm::PageMappingCacheType::FULLY_CACHED);
        void unmapAddresses(PartialHandle handle, mm::virt_addr base, size_t rangeSize);

        void addStructureToComposite(CompositeHandle, PartialHandle);
        void removeStructureFromComposite(CompositeHandle, PartialHandle);

        bool isPagePresent(PartialHandle handle, mm::virt_addr addr);
        bool isPagePresent(CompositeHandle handle, mm::virt_addr addr);
        bool wasPageAccessed(PartialHandle handle, mm::virt_addr addr);
        bool wasPageAccessed(CompositeHandle handle, mm::virt_addr addr);
        mm::PageSize getPageSize(PartialHandle handle, mm::virt_addr addr);
        mm::PageSize getPageSize(CompositeHandle handle, mm::virt_addr addr);
        mm::PageMappingPermissions getPagePermissions(PartialHandle handle, mm::virt_addr addr);
        mm::PageMappingPermissions getPagePermissions(CompositeHandle handle, mm::virt_addr addr);
        mm::PageMappingCacheType getPageCachingPolicy(PartialHandle handle, mm::virt_addr addr);
        mm::PageMappingCacheType getPageCachingPolicy(CompositeHandle handle, mm::virt_addr addr);

        //Only VirtualAddressZones should be modifying their mappings, so we only expose methods to modify mappings
        //using the PartialHandle
        void resetAccessFlag(PartialHandle handle, mm::virt_addr addr);
        void setAccessFlag(PartialHandle handle, mm::virt_addr addr);
        void setPagePermissions(PartialHandle handle, mm::virt_addr addr, mm::PageMappingPermissions);
        void setPageCachingPolicy(PartialHandle handle, mm::virt_addr addr, mm::PageMappingCacheType);
        void setPagePermissions(PartialHandle handle, mm::virt_addr base, size_t rangeSize, mm::PageMappingPermissions);
        void setPageCachingPolicy(PartialHandle handle, mm::virt_addr base, size_t rangeSize, mm::PageMappingCacheType);

        //Installs the page structure in the current running processor
        void installPageStructure(CompositeHandle handle);
        CompositeHandle getCurrentPageStructure();

        //Changes the flush planner for the current running processor only
        void pushFlushPlanner(mm::FlushPlanner& planner);
        void popFlushPlanner();

        //Exposing various ways of flushing the TLB with differing degrees of granularity
        void invltlb(bool flushGlobalTlb);
        void invlpg(mm::virt_addr addr);
        void invlpcid(uint32_t pcid);

        //Runs processor-local cleanup on the overflow pool. May perform page flushes, so this is not necessarily
        //a cheap operation
        void processOverflowPool();
        //Preallocate some page tables if necessary - potentially expensive operation.
        void topUpReservePool();

        void runSillyTest();

        //EVIL HACK (Okay it's not that evil, but it's inefficient and we should use a more proper mapping mechanism)
        void* temporaryHackMapMMIOPage(mm::phys_addr paddr);
    }

    namespace interrupts{
        struct InterruptFrame {
            // General-purpose registers, manually pushed by the stub
            uint64_t r15;
            uint64_t r14;
            uint64_t r13;
            uint64_t r12;
            uint64_t r11;
            uint64_t r10;
            uint64_t r9;
            uint64_t r8;
            uint64_t rbp;
            uint64_t rsi;
            uint64_t rdi;
            uint64_t rdx;
            uint64_t rcx;
            uint64_t rbx;
            uint64_t rax;

            uint64_t vector_index;
            // Error code (real or dummy, always pushed)
            uint64_t error_code;

            // Automatically pushed by the CPU
            uint64_t rip;
            uint64_t cs;
            uint64_t rflags;
            uint64_t rsp;
            uint64_t ss;
        };

        void init();

        class InterruptDisabler {
            bool reenable;
        public:
            InterruptDisabler();
            ~InterruptDisabler();
        };
    }
}

Core::PrintStream& operator<<(Core::PrintStream& ps, kernel::amd64::interrupts::InterruptFrame& iframe);

#endif //CROCOS_AMD64_TABLES_H
