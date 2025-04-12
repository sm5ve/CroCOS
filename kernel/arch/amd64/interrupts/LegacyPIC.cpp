//
// Created by Spencer Martin on 4/11/25.
//
#include <arch/amd64/amd64.h>

//Taken from https://wiki.osdev.org/8259_PIC
#define PIC1		0x20		/* IO base address for master PIC */
#define PIC2		0xA0		/* IO base address for slave PIC */
#define PIC1_COMMAND	PIC1
#define PIC1_DATA	(PIC1+1)
#define PIC2_COMMAND	PIC2
#define PIC2_DATA	(PIC2+1)

namespace kernel::amd64::interrupts{
    void disableLegacyPIC(){
        amd64::outb(PIC1_DATA, 0xff);
        amd64::outb(PIC2_DATA, 0xff);
    }
}