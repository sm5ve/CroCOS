//
// Created by Spencer Martin on 2/12/25.
//

#ifndef CROCOS_AMD64_TABLES_H
#define CROCOS_AMD64_TABLES_H

#include <arch/hal.h>
#include <arch/spinlock.h>
#include <kconfig.h>
#include <kernel.h>

namespace kernel::amd64 {
    //Derived from https://wiki.osdev.org/CPUID, or tables 3-19 and 3-20 in volume 2 of the Intel manual.
    enum CPUID_FEAT_LEAF_BITMAP{
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

    struct GeneralRegisterFile{

    };

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
    bool atomic_cmpxchg_u64(volatile uint64_t &var, volatile uint64_t &expected, uint64_t desired);
    bool atomic_cmpxchg_u32(volatile uint32_t &var, volatile uint32_t &expected, uint32_t desired);
    bool atomic_cmpxchg_u16(volatile uint16_t &var, volatile uint16_t &expected, uint16_t desired);
    bool atomic_cmpxchg_u8(volatile uint8_t &var, volatile uint8_t &expected, uint8_t desired);
    void atomic_and(volatile uint64_t &var, uint64_t mask);
    void atomic_or(volatile uint64_t &var, uint64_t mask);
    void invlpg(uint64_t addr);

    void hwinit();

    void acquire_spinlock(kernel::hal::spinlock_t& lock);
    bool try_acquire_spinlock(kernel::hal::spinlock_t& lock);
    void release_spinlock(kernel::hal::spinlock_t& lock);

    inline void mfence() {
        asm volatile("mfence" ::: "memory");
    }

    inline constexpr mm::virt_addr early_boot_phys_to_virt(mm::phys_addr x){
        return mm::virt_addr(x.value + VMEM_OFFSET);
    }

    inline constexpr mm::phys_addr early_boot_virt_to_phys(mm::virt_addr x){
        return mm::phys_addr(x.value - VMEM_OFFSET);
    }
}

#endif //CROCOS_AMD64_TABLES_H
