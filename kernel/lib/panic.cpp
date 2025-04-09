//
// Created by Spencer Martin on 2/15/25.
//

#include <kernel.h>

namespace kernel{

    void print_stacktrace(){
        uintptr_t* rbp;
        asm volatile("movq %%rbp, %0" : "=r"(rbp));
        kernel::DbgOut << "Stack trace:\n";

        for (int i = 0; (i < 20) && rbp; i++) {
            uintptr_t rip = rbp[1];
            kernel::DbgOut << "[" << i << "] " << (void*)rip << "\n";
            rbp = (uintptr_t*)rbp[0];
        }
    }

    [[noreturn]]
    void panic(const char* message, const char* filename, const uint32_t line){
        kernel::DbgOut << "Panic: " << message << "\n";
        kernel::DbgOut << "In file " << filename << " line " << line << "\n";
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

