//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_ARCH_H
#define CROCOS_ARCH_H

#include "stdint.h"
#include "stddef.h"
#include <arch/PageTableSpecification.h>

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#endif

namespace arch{
    void serialOutputString(const char* str);

#ifdef __x86_64__
    using ProcessorID = amd64::ProcessorID;
    constexpr size_t MAX_PROCESSOR_COUNT = 256;
    constexpr size_t CACHE_LINE_SIZE = 64;
    using InterruptFrame = amd64::interrupts::InterruptFrame;
    constexpr auto enableInterrupts = amd64::sti;
    constexpr auto disableInterrupts = amd64::cli;
    constexpr auto areInterruptsEnabled = amd64::interrupts::areInterruptsEnabled;
    constexpr size_t CPU_INTERRUPT_COUNT = amd64::INTERRUPT_VECTOR_COUNT;
    constexpr auto pageTableDescriptor = amd64::pageTableDescriptor;
#endif

    template <size_t level>
    using PTE = PageTableEntry<pageTableDescriptor.levels[level]>;
    template <size_t level>
    using PageTable = PTE<level>[pageTableDescriptor.entryCount[level]];

    //Guaranteed to be between 0 and (the total number of logical processors - 1)
    ProcessorID getCurrentProcessorID();
    size_t processorCount();

    class SerialPrintStream : public Core::PrintStream{
    protected:
        void putString(const char*) override;
    };

    class InterruptDisabler {
    private:
        bool wasEnabled;
        bool active;
    public:
        InterruptDisabler();
        ~InterruptDisabler();

        void release();
    };
}

#endif //CROCOS_ARCH_H
