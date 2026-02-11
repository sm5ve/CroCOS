//
// Created by Spencer Martin on 4/29/25.
//
#include <core/atomic.h>

#ifdef KERNEL
#include <arch.h>
#if defined(SUPPORTS_SPINLOCK_DEADLOCK_DETECTION) && defined(DEBUG_BUILD)
#define USE_SPINLOCK_DEADLOCK_DETECTION
#endif
#endif

void Spinlock::acquire() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    auto meta = metadata.load(ACQUIRE);
    if (meta & activeMeta) {
        assert ((meta & 0xff) != arch::debugEarlyBootCPUID(), "Deadlock detected!");
    }
#endif
    while (!locked.compare_exchange_v(false, true, ACQUIRE)) {
        tight_spin();
    }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
}

void Spinlock::release() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(0);
#endif
    locked.store(false, RELEASE);
}

bool Spinlock::try_acquire() {
    if (locked.compare_exchange_v(false, true, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }
    return false;
}

const uint64_t write_lock_queued_bit = 1 << 1;
const uint64_t write_lock_acquired_bit = 1 << 0;
const uint64_t write_lock_mask = write_lock_acquired_bit | write_lock_queued_bit;
const uint64_t read_lock_count_shift = 2;

void RWSpinlock::acquire_reader(){
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    auto meta = metadata.load(ACQUIRE);
    if (meta & activeMeta) {
        assert ((meta & 0xff) != arch::debugEarlyBootCPUID(), "Deadlock detected!");
    }
#endif
    lockstate.update_and_get_when([](uint64_t state){
        return (state & write_lock_mask) == 0;
    }, [](uint64_t state){
        uint64_t count = state >> read_lock_count_shift;
        return (count + 1) << read_lock_count_shift;
    });
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
}

bool RWSpinlock::try_acquire_reader(){
    uint64_t count = lockstate >> read_lock_count_shift;
    uint64_t newCount = count + 1;
    uint64_t expectedValue = count << read_lock_count_shift;
    if (lockstate.compare_exchange(expectedValue, newCount << read_lock_count_shift)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }
    return false;
}

void RWSpinlock::acquire_writer(){
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    auto meta = metadata.load(ACQUIRE);
    if (meta & activeMeta) {
        assert ((meta & 0xff) != arch::debugEarlyBootCPUID(), "Deadlock detected!");
    }
#endif
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
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
}

bool RWSpinlock::try_acquire_writer(){
    if (lockstate.compare_exchange_v(0, 1)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(arch::debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }
    return false;
}

void RWSpinlock::release_reader(){
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(0);
#endif
    lockstate.update_and_get([](uint64_t state){
        uint64_t writerState = state & write_lock_mask;
        uint64_t readCount = state >> read_lock_count_shift;
        return ((readCount - 1) << read_lock_count_shift) | writerState;
    });
}

void RWSpinlock::release_writer(){
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
    metadata.store(0);
#endif
    lockstate &= ~write_lock_acquired_bit;
}

bool RWSpinlock::writer_lock_taken() const{
    return lockstate & write_lock_acquired_bit;
}

bool RWSpinlock::reader_lock_taken() const{
    return (lockstate & ~write_lock_mask) != 0;
}

bool Spinlock::lock_taken() const {
    return locked.load(ACQUIRE);
}
