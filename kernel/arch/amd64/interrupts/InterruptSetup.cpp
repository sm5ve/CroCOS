//
// Created by Spencer Martin on 4/23/25.
//

#include <arch/amd64/amd64.h>

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

    alignas(2048) IDTEntry idt[256];

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
        DbgOut << "ISR0 is at " << (void*)isr_0 << "\n";
        //actually populate the IDT using the auto-generated macro file
        #include "isr_set.inc"
        asm volatile("lidt %0" : : "m"(idtr));
        //Probably we should defer calling sti until the system is in a more stable state.
        asm volatile("sti");
        DbgOut << "About to test the interrupt handler int 0" << "\n";
        //for(;;);
        asm volatile("int $0");
        asm volatile("int $1");
        asm volatile("int $45");
        asm volatile("int $30");
        asm volatile("int $178");

        DbgOut << "Done" << "\n";
        //DbgOut << *((int*) (1 - 1)) << "\n";
    }
}

extern "C" void interrupt_common_handler(kernel::amd64::interrupts::InterruptFrame& test){
    kernel::DbgOut << "The vector index is " << test.vector_index << "\n";
}