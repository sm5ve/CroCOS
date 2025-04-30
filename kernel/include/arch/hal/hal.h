//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_HAL_H
#define CROCOS_HAL_H

#include "stdint.h"
#include "stddef.h"

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#endif

namespace kernel::hal{
    void serialOutputString(const char* str);
    void hwinit();

#ifdef __x86_64__
    using ProcessorID = kernel::amd64::ProcessorID;
    const size_t MAX_PROCESSOR_COUNT = 256;
    const size_t CACHE_LINE_SIZE = 64;
    using InterruptFrame = kernel::amd64::interrupts::InterruptFrame;
#endif

    //Guaranteed to be between 0 and (the total number of logical processors - 1)
    ProcessorID getCurrentProcessorID();
    size_t processorCount();

    inline void compiler_fence(){
        asm volatile("" ::: "memory");
    };

    bool atomic_cmpxchg_u64(volatile uint64_t &var, volatile uint64_t &expected, uint64_t desired);

class SerialPrintStream : public Core::PrintStream{
    protected:
        void putString(const char*) override;
    };
}

#endif //CROCOS_HAL_H
