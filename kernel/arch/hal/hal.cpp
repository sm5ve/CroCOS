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

    void acquire_reader_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::acquire_reader_lock(lock);
#endif
    }

    void acquire_writer_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::acquire_writer_lock(lock);
#endif
    }

    bool try_acquire_reader_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        return kernel::amd64::try_acquire_reader_lock(lock);
#endif
    }

    bool try_acquire_writer_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        return kernel::amd64::try_acquire_writer_lock(lock);
#endif
    }

    void release_writer_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::release_writer_lock(lock);
#endif
    }

    void release_reader_lock(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        kernel::amd64::release_reader_lock(lock);
#endif
    }

    bool writer_lock_taken(kernel::hal::rwlock_t& lock){
#ifdef __x86_64__
        return kernel::amd64::writer_lock_taken(lock);
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

    bool atomic_cmpxchg_u64(volatile uint64_t &var, volatile uint64_t &expected, uint64_t desired){
        return amd64::atomic_cmpxchg_u64(var, expected, desired);
    }

    void SerialPrintStream::putString(const char * str){
        kernel::hal::serialOutputString(str);
    }
}