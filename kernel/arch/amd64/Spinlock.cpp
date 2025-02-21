//
// Created by Spencer Martin on 2/12/25.
//

#include <arch/amd64.h>

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
}