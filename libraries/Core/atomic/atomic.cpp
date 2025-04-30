//
// Created by Spencer Martin on 4/29/25.
//
#include <core/atomic.h>

void Spinlock::lock() {
    while (!locked.compare_exchange_v(false, true, ACQUIRE)) {
        tight_spin();
    }
}

void Spinlock::unlock() {
    locked.store(false, RELEASE);
}

bool Spinlock::try_lock() {
    return locked.compare_exchange_v(false, true, ACQUIRE);
}