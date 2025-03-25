//
// Created by Spencer Martin on 2/15/25.
//

#include <kernel.h>

namespace kernel{
    [[noreturn]]
    void panic(const char* message, const char* filename, const uint32_t line){
        kernel::DbgOut << "Panic: " << message << "\n";
        kernel::DbgOut << "In file " << filename << " line " << line << "\n";
#ifdef __x86_64__
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
        for(;;)
            asm volatile("hlt");
#endif
    }
}

