//
// Created by Spencer Martin on 2/15/25.
//

#include <arch.h>
#include <mem/PageAllocator.h>

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#include <arch/amd64/smp.h>
#endif
extern size_t archProcessorCount;

#if defined(SUPPORTS_SPINLOCK_DEADLOCK_DETECTION) && defined(DEBUG_BUILD)
#define USE_SPINLOCK_DEADLOCK_DETECTION
#endif

namespace arch{
    void serialOutputString(const char *str) {
#ifdef __x86_64__
        amd64::serialOutputString(str);
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
        serialOutputString(str);
    }

    InterruptDisabler::InterruptDisabler() {
        active = true;
        wasEnabled = areInterruptsEnabled();
    }

    void InterruptDisabler::release() {
        if (active) {
            active = false;
            if (wasEnabled) {
                enableInterrupts();
            }
            else {
                disableInterrupts();
            }
        }
    }

    InterruptDisabler::~InterruptDisabler() {
        release();
    }

    IteratorRange<MemMapIterator> getMemoryMap() {
#ifdef ARCH_AMD64
        return amd64::getMemoryMap();
#endif
    }

    InterruptDisablingSpinlock::InterruptDisablingSpinlock(){
        state = InterruptState::STALE;
        acquired = false;
        metadata = 0;
    }

    constexpr size_t activeMeta = 1ul << 63;

    void InterruptDisablingSpinlock::acquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
            while (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
                enableInterrupts();
                tight_spin();
                disableInterrupts();
            }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        }
        else {
            acquirePlain();
        }
        state = newState;
    }

    void InterruptDisablingSpinlock::acquirePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif
        while (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
            tight_spin();
        }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        state = InterruptState::STALE;
    }

    bool InterruptDisablingSpinlock::tryAcquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();
        if (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
            //If we fail to acquire the lock and interrupts were originally enabled, reenable them
            if (newState == InterruptState::ENABLED) enableInterrupts();
            return false;
        }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }

    bool InterruptDisablingSpinlock::tryAcquirePlain() {
        if (acquired.compare_exchange_v(false, true, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
            state = InterruptState::STALE;
            return true;
        }
        return false;
    }

    void InterruptDisablingSpinlock::release() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        auto oldState = state;
        state = InterruptState::STALE;
        acquired.store(false, RELEASE);
        if (oldState == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (oldState == InterruptState::DISABLED) {
            disableInterrupts();
        }
    }

    InterruptDisablingSpinlock::InterruptResetter InterruptDisablingSpinlock::releasePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        acquired.store(false, RELEASE);
        auto oldState = state;
        state = InterruptState::STALE;
        return {oldState};
    }

    void InterruptDisablingSpinlock::InterruptResetter::operator()() {
        if (state == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (state == InterruptState::DISABLED) {
            disableInterrupts();
        }
        state = InterruptState::STALE;
    }


}