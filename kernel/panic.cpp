//
// Created by Spencer Martin on 2/15/25.
//

#include <arch/amd64/amd64.h>

#include "kernel.h"
#include <arch.h>

namespace kernel{

    void print_stacktrace(uintptr_t* rbp) {
        if (arch::pageTableDescriptor.canonicalizeVirtualAddress(mm::virt_addr(rbp)) != mm::virt_addr(rbp)) {
            emergencyLog() << "cannot produce stacktrace since rbp is noncanonical\n";
        }
        emergencyLog() << "Stack trace:\n";

        for (int i = 0; (i < 20) && rbp; i++) {
            uintptr_t rip = rbp[1];
            emergencyLog() << "[" << i << "] " << (void*)rip << "\n";
            rbp = (uintptr_t*)rbp[0];
            if (arch::pageTableDescriptor.canonicalizeVirtualAddress(mm::virt_addr(rbp)) != mm::virt_addr(rbp)) {
                emergencyLog() << "cannot continue stacktrace since rbp is now noncanonical\n";
                break;
            }
        }
    }

    void print_stacktrace(){
        uintptr_t* rbp;
        asm volatile("movq %%rbp, %0" : "=r"(rbp));
        print_stacktrace(rbp);
    }
}

