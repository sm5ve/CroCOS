//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"
#include <panic.h>

namespace kernel::amd64{
    const uint64_t write_lock_queued_bit = 1 << 1;
    const uint64_t write_lock_acquired_bit = 1 << 0;
    const uint64_t write_lock_mask = write_lock_acquired_bit | write_lock_queued_bit;
    const uint64_t read_lock_count_shift = 2;

    void acquire_reader_lock(kernel::hal::rwlock_t& lock){
        //print_stacktrace();
        while(true){
            //Wait for all writers to release the lock and for no writer to request the lock
            while((lock.lock_bit & write_lock_mask) != 0){
                asm volatile("pause");
            }
            uint64_t count = lock.lock_bit >> read_lock_count_shift;
            uint64_t newCount = count + 1;
            //We expect the lock value to have no writers active or queued
            uint64_t expectedValue = count << read_lock_count_shift;
            if(atomic_cmpxchg_u64(lock.lock_bit, expectedValue, newCount << read_lock_count_shift)){
                return;
            }
        }
    }

    void acquire_writer_lock(kernel::hal::rwlock_t& lock){
        //print_stacktrace();
        while(true){
            while((lock.lock_bit & write_lock_queued_bit) != 0){
                asm volatile("pause");
            }
            uint64_t expectedLockValue = (lock.lock_bit & ~write_lock_queued_bit);
            uint64_t newLockValue = (expectedLockValue | write_lock_queued_bit);
            //Signal our intent to take the writer lock
            if(atomic_cmpxchg_u64(lock.lock_bit, expectedLockValue, newLockValue)){
                while(true){
                    //Wait for nobody else to hold the lock
                    while((lock.lock_bit & ~write_lock_queued_bit) != 0){
                        asm volatile("pause");
                    }
                    expectedLockValue = write_lock_queued_bit;
                    newLockValue = write_lock_acquired_bit;
                    //Take the writer lock
                    if(atomic_cmpxchg_u64(lock.lock_bit, expectedLockValue, newLockValue)){
                        return;
                    }
                }
            }
        }
    }

    bool try_acquire_reader_lock(kernel::hal::rwlock_t& lock){
        uint64_t count = lock.lock_bit >> read_lock_count_shift;
        uint64_t newCount = count + 1;
        uint64_t expectedValue = count << read_lock_count_shift;
        return atomic_cmpxchg_u64(lock.lock_bit, expectedValue, newCount << read_lock_count_shift);
    }

    bool try_acquire_writer_lock(kernel::hal::rwlock_t& lock){
        uint64_t expectedValue = 0;
        return atomic_cmpxchg_u64(lock.lock_bit, expectedValue, 1);
    }

    void release_writer_lock(kernel::hal::rwlock_t& lock){
        atomic_and(lock.lock_bit, ~write_lock_acquired_bit);
    }

    void release_reader_lock(kernel::hal::rwlock_t& lock){
        assert((lock.lock_bit & ~write_lock_mask) != 0, "tried to release reader lock when no reader held lock");
        while(true){
            uint64_t oldLock = lock.lock_bit;
            uint64_t newValue = lock.lock_bit & write_lock_mask;
            uint64_t oldCount = oldLock >> read_lock_count_shift;
            newValue |= (oldCount - 1) << read_lock_count_shift;
            if(atomic_cmpxchg_u64(lock.lock_bit, oldLock, newValue)){
                return;
            }
        }
    }

    bool writer_lock_taken(kernel::hal::rwlock_t& lock){
        return (lock.lock_bit & write_lock_acquired_bit) != 0;
    }
}