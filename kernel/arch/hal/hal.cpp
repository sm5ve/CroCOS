//
// Created by Spencer Martin on 2/15/25.
//

#include "arch/hal/hal.h"

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#include <arch/amd64/smp.h>
#endif
extern size_t archProcessorCount;

namespace kernel::hal{
    void serialOutputString(const char *str) {
#ifdef __x86_64__
        kernel::amd64::serialOutputString(str);
#endif
    }

    void hwinit(){
#ifdef __x86_64__
        kernel::amd64::hwinit();
#endif
    }

    size_t processorCount(){
        return archProcessorCount;
    }

    ProcessorID getCurrentProcessorID(){
#ifdef __x86_64__
        return amd64::smp::getLogicalProcessorID();
#endif
    }

    void SerialPrintStream::putString(const char * str){
        kernel::hal::serialOutputString(str);
    }
}