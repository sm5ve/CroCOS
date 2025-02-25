//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_HAL_H
#define CROCOS_HAL_H

#include "stdint.h"
#include "stddef.h"

#define TOCTOU_LOCK_CHECK(lock, condition, action, failure) \
if (condition) { \
acquire_spinlock(lock); \
if (condition) { \
action; \
} else { \
release_spinlock(lock); \
failure;\
} \
}\
else {\
failure;\
}
namespace kernel::hal{
    void serialOutputString(const char* str);
    void hwinit();

    typedef struct alignas(64){
        uint64_t lock_bit;
        uint64_t acquire_count;
#ifdef __x86_64__
        uint64_t padding[6]; //make sure we take up an entire cache line to minimize bus traffic (https://wiki.osdev.org/Spinlock)
#endif
    } spinlock_t;

    const spinlock_t SPINLOCK_INITIALIZER = {0, 0, {0, 0, 0, 0, 0, 0}};

    void acquire_spinlock(kernel::hal::spinlock_t& lock);
    //Returns true if we were able to acquire the lock, and false otherwise
    bool try_acquire_spinlock(kernel::hal::spinlock_t& lock);
    void release_spinlock(kernel::hal::spinlock_t& lock);

#ifdef __x86_64__
    using ProcessorID = uint8_t;
    const size_t MAX_PROCESSOR_COUNT = 256;
    const size_t CACHE_LINE_SIZE = 64;
#endif

    //Guaranteed to be between 0 and (the total number of logical processors - 1)
    ProcessorID getCurrentProcessorID();
    size_t processorCount();
}

#endif //CROCOS_HAL_H
