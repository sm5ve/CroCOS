//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_HAL_H
#define CROCOS_HAL_H

#include "stdint.h"
#include "stddef.h"
#include <arch/spinlock.h>

#ifdef __x86_64__
#include <arch/amd64.h>
#endif

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
    void acquire_spinlock(kernel::hal::spinlock_t& lock);
    //Returns true if we were able to acquire the lock, and false otherwise
    bool try_acquire_spinlock(kernel::hal::spinlock_t& lock);
    void release_spinlock(kernel::hal::spinlock_t& lock);

    void acquire_reader_lock(kernel::hal::rwlock_t& lock);
    void acquire_writer_lock(kernel::hal::rwlock_t& lock);
    bool try_acquire_reader_lock(kernel::hal::rwlock_t& lock);
    bool try_acquire_writer_lock(kernel::hal::rwlock_t& lock);
    void release_writer_lock(kernel::hal::rwlock_t& lock);
    void release_reader_lock(kernel::hal::rwlock_t& lock);
    bool writer_lock_taken(kernel::hal::rwlock_t& lock);

#ifdef __x86_64__
    using ProcessorID = uint8_t;
    const size_t MAX_PROCESSOR_COUNT = 256;
    const size_t CACHE_LINE_SIZE = 64;
    //using GeneralRegisterFile = kernel::amd64::GeneralRegisterFile;
#endif

    //Guaranteed to be between 0 and (the total number of logical processors - 1)
    ProcessorID getCurrentProcessorID();
    size_t processorCount();

    inline void compiler_fence(){
        asm volatile("" ::: "memory");
    };

    bool atomic_cmpxchg_u64(volatile uint64_t &var, volatile uint64_t &expected, uint64_t desired);
}

#endif //CROCOS_HAL_H
