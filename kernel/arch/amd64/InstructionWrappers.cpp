//
// Created by Spencer Martin on 2/12/25.
//

#include <arch/amd64.h>

namespace kernel::amd64 {
    //Wrapper for the CPUID instruction. The first four parameters are references to uint32_t's corresponding to
    //the registers EAX-EDX. The last parameter is the value to load into EAX (the "leaf" per the
    //Intel manual) before calling CPUID.
    void cpuid(uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx, uint32_t leaf) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf));
    }

    void outb(uint16_t port, uint8_t out)
    {
        asm volatile("outb %0, %1" ::"a"(out), "Nd"(port));
    }

    uint8_t inb(uint16_t port)
    {
        uint8_t out;
        asm volatile("inb %1, %0"
        : "=a"(out)
        : "Nd"(port));
        return out;
    }

    void outw(uint16_t port, uint16_t word)
    {
        asm volatile("outw %0, %1" ::"a"(word), "Nd"(port));
    }

    uint16_t inw(uint16_t port)
    {
        uint16_t out;
        asm volatile("inw %1, %0"
        : "=a"(out)
        : "Nd"(port));
        return out;
    }

    void cli() { asm volatile("cli"); }

    void sti() { asm volatile("sti"); }
}