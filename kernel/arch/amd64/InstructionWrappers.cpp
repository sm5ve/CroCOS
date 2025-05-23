//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"

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

    bool atomic_cmpxchg_u64(volatile uint64_t &var, volatile uint64_t &expected, uint64_t desired) {
        uint8_t success;
        __asm__ volatile (
                "lock; cmpxchgq %3, %1\n"
                "sete %2"
                : "+a" (expected), "+m" (var), "=r" (success)
                : "r" (desired)
                : "memory", "cc"
                );
        return success;
    }

    bool atomic_cmpxchg_u32(volatile uint32_t &var, volatile uint32_t &expected, uint32_t desired) {
        uint8_t success;
        __asm__ volatile (
                "lock; cmpxchgl %3, %1\n"
                "sete %2"
                : "+a" (expected), "+m" (var), "=r" (success)
                : "r" (desired)
                : "memory", "cc"
                );
        return success;
    }

    bool atomic_cmpxchg_u16(volatile uint16_t &var, volatile uint16_t &expected, uint16_t desired) {
        uint8_t success;
        __asm__ volatile (
                "lock; cmpxchgw %3, %1\n"
                "sete %2"
                : "+a" (expected), "+m" (var), "=r" (success)
                : "r" (desired)
                : "memory", "cc"
                );
        return success;
    }

    bool atomic_cmpxchg_u8(volatile uint8_t &var, volatile uint8_t &expected, uint8_t desired) {
        uint8_t success;
        __asm__ volatile (
                "lock; cmpxchgb %3, %1\n"
                "sete %2"
                : "+a" (expected), "+m" (var), "=r" (success)
                : "r" (desired)
                : "memory", "cc"
                );
        return success;
    }

    void atomic_and(volatile uint64_t &var, uint64_t mask) {
        __asm__ volatile (
                "lock; andq %1, %0"
                : "+m" (var)    // Output: memory operand (read/write)
                : "r" (mask)    // Input: register operand
                : "memory"      // Clobber: tells the compiler memory is modified
                );
    }

    void atomic_or(volatile uint64_t &var, uint64_t mask) {
        __asm__ volatile (
                "lock; orq %1, %0"
                : "+m" (var)    // Output: memory operand (read/write)
                : "r" (mask)    // Input: register operand
                : "memory"      // Clobber: tells the compiler memory is modified
                );
    }

    void invlpg(uint64_t addr){
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    }
}