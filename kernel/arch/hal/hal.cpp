//
// Created by Spencer Martin on 2/15/25.
//

#include <arch/hal.h>

#ifdef __x86_64__
#include <arch/amd64.h>
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

    void acquire_spinlock(kernel::hal::spinlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::acquire_spinlock(lock);
#endif
    }

    bool try_acquire_spinlock(kernel::hal::spinlock_t& lock){
#ifdef __x86_64__
        return kernel::amd64::try_acquire_spinlock(lock);
#endif
    }

    void release_spinlock(kernel::hal::spinlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::release_spinlock(lock);
#endif
    }

    size_t processorCount(){
        return archProcessorCount;
    }

    ProcessorID getCurrentProcessorID(){
        //TODO IMPLEMENT
        //This will require some sort of processor-local memory page to implement efficiently. But that's okay!
        //We'll cross that bridge when we get to it :)
        return 0;
    }
}