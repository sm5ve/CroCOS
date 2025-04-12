//
// Created by Spencer Martin on 4/12/25.
//

#include <arch/amd64/smp.h>

namespace kernel::amd64::smp{
    void setLogicalProcessorID(hal::ProcessorID pid){
        static_assert(sizeof(hal::ProcessorID) <= sizeof(uint8_t), "You need to update your use of gsbase to support a larger processor ID");
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        currentGsBase &= ~0xfful;
        currentGsBase |= (uint64_t)pid;
        asm volatile("wrgsbase %0" : : "r"(currentGsBase));
    }

    hal::ProcessorID getLogicalProcessorID(){
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        return currentGsBase & 0xff;
    }
}