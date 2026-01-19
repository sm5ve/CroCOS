//
// Created by Spencer Martin on 4/23/25.
//

#include <arch/amd64/amd64.h>
#include <interrupts/interrupts.h>

#define ISR(n) extern "C" void isr_##n();
#define SET_ISR(n) set_idt_entry(n, isr_##n, is_trap_gate(n));

namespace arch::amd64::interrupts{
    //Use the python-generated string collection of macros to get access to our various ISRs
    #include "isr.inc"

    struct IDTEntry {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t zero;
    } __attribute__((packed));

    IDTEntry idt[256];

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
            .limit = sizeof(idt) - 1,
            .base = (uint64_t)&idt
    };

    constexpr bool is_trap_gate(int vector) {
        switch (vector) {
            case 0: case 1: case 3: case 4: case 5:
            case 6: case 7: case 16: case 17:
            case 18: case 19: case 20:
                return true;
            default:
                return false;
        }
    }

    void set_idt_entry(int vector, void (*handler)(), bool trap) {
        uint8_t type_attr = trap ? 0x8F: 0x8E;
        uintptr_t addr = reinterpret_cast<uintptr_t>(handler);
        idt[vector] = {
                .offset_low  = (uint16_t)(addr & 0xFFFF),
                .selector    = 0x08,               // kernel code segment
                .ist         = 0,
                .type_attr   = type_attr,          // Present, DPL=0, Interrupt/Trap Gate
                .offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF),
                .offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF),
                .zero        = 0
        };
    }

    bool initBSP(){
        arch::amd64::cli();
        //actually populate the IDT using the auto-generated macro file
        #include "isr_set.inc"
        asm volatile("lidt %0" : : "m"(idtr));
        return true;
    }

    bool initAP() {
        cli();
        asm volatile("lidt %0" : : "m"(idtr));
        return true;
    }

    bool areInterruptsEnabled() {
        uint64_t rflags;
        asm volatile("pushfq; pop %0" : "=r"(rflags));
        return rflags & (1 << 9);
    }
}

Core::PrintStream& operator<<(Core::PrintStream& ps, arch::amd64::interrupts::InterruptFrame& iframe){
    ps << "Interrupt frame for vector " << iframe.vector_index << " error code " << iframe.error_code << "\n";
    ps << "RIP " << (void*)iframe.rip << "    ";
    ps << "FLG " << (void*)iframe.rflags << "    ";
    ps << "CS  " << (void*)iframe.cs << "    ";
    ps << "SS  " << (void*)iframe.ss << "    " << "\n";
    ps << "RAX " << (void*)iframe.rax << "    ";
    ps << "RBX " << (void*)iframe.rbx << "    ";
    ps << "RCX " << (void*)iframe.rcx << "    ";
    ps << "RDX " << (void*)iframe.rdx << "    " << "\n";
    ps << "RDI " << (void*)iframe.rdi << "    ";
    ps << "RSI " << (void*)iframe.rsi << "    ";
    ps << "RBP " << (void*)iframe.rbp << "    ";
    ps << "RSP " << (void*)iframe.rsp << "    " << "\n";
    ps << "R8  " << (void*)iframe.r8 << "    ";
    ps << "R9  " << (void*)iframe.r9 << "    ";
    ps << "R10 " << (void*)iframe.r10 << "    ";
    ps << "R11 " << (void*)iframe.r11 << "    " << "\n";
    ps << "R12 " << (void*)iframe.r12 << "    ";
    ps << "R13 " << (void*)iframe.r13 << "    ";
    ps << "R14 " << (void*)iframe.r14 << "    ";
    ps << "R15 " << (void*)iframe.r15 << "    " << "\n";
    return ps;
}

extern "C" void interrupt_common_handler(arch::amd64::interrupts::InterruptFrame& frame){
    interrupts::managed::dispatchInterrupt(frame);
    //kernel::print_stacktrace(reinterpret_cast<uintptr_t*>(&frame.rbp));
}
