//
// Created by Spencer Martin on 2/15/25.
//

#include <kernel.h>

namespace kernel{

    void print_stacktrace(){
        uintptr_t* rbp;
        asm volatile("movq %%rbp, %0" : "=r"(rbp));
        kernel::klog << "Stack trace:\n";

        for (int i = 0; (i < 20) && rbp; i++) {
            uintptr_t rip = rbp[1];
            kernel::klog << "[" << i << "] " << (void*)rip << "\n";
            rbp = (uintptr_t*)rbp[0];
        }
    }
}

