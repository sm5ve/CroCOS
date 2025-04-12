//
// Created by Spencer Martin on 2/13/25.
//
#include "arch/hal/hal.h"
#include <kernel.h>
#include <lib/ds/Vector.h>
#include <lib/ds/SmartPointer.h>

#include <lib/ds/HashMap.h>

namespace kernel{
    SerialPrintStream EarlyBootStream;
    PrintStream& DbgOut = EarlyBootStream;

    extern "C" void kernel_main(){
        DbgOut << "\n"; // newline to separate from the "Booting from ROM.." message from qemu

        DbgOut << "Hello amd64 kernel world!\n";

        hal::hwinit();

        DbgOut << "init done!\n";

        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}