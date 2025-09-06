//
// Created by Spencer Martin on 4/23/25.
//

#include <arch/amd64/amd64.h>
#include <arch/hal/interrupts.h>

#define ISR(n) extern "C" void isr_##n();
#define SET_ISR(n) set_idt_entry(n, isr_##n, is_trap_gate(n));

namespace kernel::amd64::interrupts{
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

    void init(){
        //actually populate the IDT using the auto-generated macro file
        #include "isr_set.inc"
        asm volatile("lidt %0" : : "m"(idtr));
        //Probably we should defer calling sti until the system is in a more stable state.
        //asm volatile("sti");
    }

    InterruptDisabler::InterruptDisabler() {
        uint64_t rflags;
        asm volatile("pushfq; pop %0" : "=r"(rflags));
        if (rflags & (1 << 9))
            reenable = true;
        else
            reenable = false;
        cli();
    }

    InterruptDisabler::~InterruptDisabler() {
        if (reenable)
            sti();
    }


}

Core::PrintStream& operator<<(Core::PrintStream& ps, kernel::amd64::interrupts::InterruptFrame& iframe){
    kernel::klog << "Interrupt frame for vector " << iframe.vector_index << " error code " << iframe.error_code << "\n";
    kernel::klog << "RIP " << (void*)iframe.rip << "    ";
    kernel::klog << "FLG " << (void*)iframe.rflags << "    ";
    kernel::klog << "CS  " << (void*)iframe.cs << "    ";
    kernel::klog << "SS  " << (void*)iframe.ss << "    " << "\n";
    kernel::klog << "RAX " << (void*)iframe.rax << "    ";
    kernel::klog << "RBX " << (void*)iframe.rbx << "    ";
    kernel::klog << "RCX " << (void*)iframe.rcx << "    ";
    kernel::klog << "RDX " << (void*)iframe.rdx << "    " << "\n";
    kernel::klog << "RDI " << (void*)iframe.rdi << "    ";
    kernel::klog << "RSI " << (void*)iframe.rsi << "    ";
    kernel::klog << "RBP " << (void*)iframe.rbp << "    ";
    kernel::klog << "RSP " << (void*)iframe.rsp << "    " << "\n";
    kernel::klog << "R8  " << (void*)iframe.r8 << "    ";
    kernel::klog << "R9  " << (void*)iframe.r9 << "    ";
    kernel::klog << "R10 " << (void*)iframe.r10 << "    ";
    kernel::klog << "R11 " << (void*)iframe.r11 << "    " << "\n";
    kernel::klog << "R12 " << (void*)iframe.r12 << "    ";
    kernel::klog << "R13 " << (void*)iframe.r13 << "    ";
    kernel::klog << "R14 " << (void*)iframe.r14 << "    ";
    kernel::klog << "R15 " << (void*)iframe.r15 << "    " << "\n";
    return ps;
}

extern "C" void interrupt_common_handler(kernel::amd64::interrupts::InterruptFrame& frame){
    kernel::hal::interrupts::managed::dispatchInterrupt(frame);
    //kernel::print_stacktrace(reinterpret_cast<uintptr_t*>(&frame.rbp));
}
