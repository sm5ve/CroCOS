//
// Created by Spencer Martin on 4/29/25.
//
#include <core/atomic.h>

void Spinlock::acquire() {
    while (!locked.compare_exchange_v(false, true, ACQUIRE)) {
        tight_spin();
    }
}

void Spinlock::release() {
    locked.store(false, RELEASE);
}

bool Spinlock::try_acquire() {
    return locked.compare_exchange_v(false, true, ACQUIRE);
}

/*
 *

    bool try_acquire_writer_lock(kernel::hal::rwlock_t& lock){
    }

    void release_writer_lock(kernel::hal::rwlock_t& lock){
        atomic_and(lock.lock_bit, ~write_lock_acquired_bit);
    }

    void release_reader_lock(kernel::hal::rwlock_t& lock){
        assert((lock.lock_bit & ~write_lock_mask) != 0, "tried to release reader lock when no reader held acquire");
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
 */

const uint64_t write_lock_queued_bit = 1 << 1;
const uint64_t write_lock_acquired_bit = 1 << 0;
const uint64_t write_lock_mask = write_lock_acquired_bit | write_lock_queued_bit;
const uint64_t read_lock_count_shift = 2;

void RWSpinlock::acquire_reader(){
    lockstate.update_and_get_when([](uint64_t state){
        return (state & write_lock_mask) == 0;
    }, [](uint64_t state){
        uint64_t count = state >> read_lock_count_shift;
        return (count + 1) << read_lock_count_shift;
    });
}

bool RWSpinlock::try_acquire_reader(){
    uint64_t count = lockstate >> read_lock_count_shift;
    uint64_t newCount = count + 1;
    uint64_t expectedValue = count << read_lock_count_shift;
    return lockstate.compare_exchange(expectedValue, newCount << read_lock_count_shift);
}

void RWSpinlock::acquire_writer(){
    //First try to signal the intent to acquire the writer lock
    lockstate.update_and_get_when([](uint64_t state){
        return (state & write_lock_queued_bit) == 0;
    }, [](uint64_t state){
        return state | write_lock_queued_bit;
    });
    //Then wait for the reader count to hit 0 before acquiring the writer lock
    lockstate.update_and_get_when([](uint64_t state){
        return state == write_lock_queued_bit;
    }, [](uint64_t state){
        (void)state;
       return write_lock_acquired_bit;
    });
}

bool RWSpinlock::try_acquire_writer(){
    return lockstate.compare_exchange_v(0, 1);
}

void RWSpinlock::release_reader(){
    lockstate.update_and_get([](uint64_t state){
        uint64_t writerState = state & write_lock_mask;
        uint64_t readCount = state >> read_lock_count_shift;
        return ((readCount - 1) << read_lock_count_shift) | writerState;
    });
}

void RWSpinlock::release_writer(){
    lockstate &= ~write_lock_acquired_bit;
}

bool RWSpinlock::writer_lock_taken() {
    return lockstate & write_lock_acquired_bit;
}

bool RWSpinlock::reader_lock_taken() {
    return (lockstate & ~write_lock_mask) != 0;
}