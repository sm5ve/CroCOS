//
// Created by Spencer Martin on 2/12/25.
//

#include <arch/amd64.h>
#include <panic.h>

namespace kernel::amd64{
    void acquire_spinlock(kernel::hal::spinlock_t& lock){
        //TODO add an optional safeguard for a CPU core double-acquiring a lock specified by a flag in the structure
        //Again referenced from the ever-wonderful https://wiki.osdev.org/Spinlock
        asm volatile(
                ".acquire_body:\n"
                "lock btsq $0, (%%rax)\n" //attempt to acquire the lock with an atomic operation
                "jc .spin_with_pause\n" //if we failed to acquire the lock, jump to our spin loop
                "incq (%%rbx)\n" //otherwise, increment the acquire_count for performance counting reasons
                "jmp .spin_exit\n"
                ".spin_with_pause:\n"
                "pause\n" //apparently a useful hint to hyperthreaded CPUs
                "testq $1, (%%rax)\n" //check if the lock has been freed
                "jnz .spin_with_pause\n" //if not, loop!
                "jmp .acquire_body\n" //otherwise, try to acquire the lock (and hope nobody beat you to the punch!)
                ".spin_exit:"
                :: "a"(&lock.lock_bit), "b"(&lock.acquire_count)
                );
    }

    bool try_acquire_spinlock(kernel::hal::spinlock_t& lock){
        bool succeeded;
        // Using lock btsq to atomically set the lock bit
        asm volatile(
                "lock btsq $0, (%%rax)\n"          // Attempt to acquire the lock with an atomic operation
                "jc .try_lock_acquire_fail\n"      // Jump to failure if the lock is already acquired (carry set)
                "mov $1, %%rbx\n"                 // Set succeeded flag (1 for success)
                "jmp .try_lock_acquire_end\n"     // Jump to end
                ".try_lock_acquire_fail:\n"
                "mov $0, %%rbx\n"                 // Set succeeded flag to 0 if failed
                ".try_lock_acquire_end:"
                : "=b" (succeeded)                // Output the result to succeeded
                : "a" (&lock.lock_bit)           // Input: address of the lock bit
                );
        return succeeded;
    }


    void release_spinlock(kernel::hal::spinlock_t& lock){
        //TODO add a safeguard for a CPU core releasing a lock it doesn't own
        lock.lock_bit = 0; //In x86/amd64, uint32_t-aligned writes are atomic! So no need to do weird inline assembly
    }

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