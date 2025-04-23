//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_PANIC_H
#define CROCOS_PANIC_H

#include "kernel.h"

#define PANIC(...) kernel::panic(__FILE__, __LINE__, __VA_ARGS__)

namespace kernel{
    void print_stacktrace();
    template <typename... Args>
    [[noreturn]]
    void panic(const char* filename, const uint32_t line, Args&&... args){
        kernel::DbgOut << "Panic: ";
        (kernel::DbgOut << ... << forward<Args>(args));
        kernel::DbgOut << "\nIn file " << filename << " line " << line << "\n";
        print_stacktrace();
#ifdef __x86_64__
        //Give QEMU some time to actually print the panic message before quitting
        for(auto i = 0; i < 1000; i++){
            asm volatile("pause");
        }
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
        for(;;)
            asm volatile("hlt");
#endif
    }
}

#endif //CROCOS_PANIC_H