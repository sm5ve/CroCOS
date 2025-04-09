//
// Created by Spencer Martin on 3/9/25.
//

#ifndef CROCOS_SPINLOCK_H
#define CROCOS_SPINLOCK_H

#include "stdint.h"

namespace kernel::hal {
    typedef struct alignas(64) {
        uint64_t lock_bit;
        uint64_t acquire_count;
#ifdef __x86_64__
        uint64_t padding[6]; //make sure we take up an entire cache line to minimize bus traffic (https://wiki.osdev.org/Spinlock)
#endif
    } spinlock_t;

    const spinlock_t SPINLOCK_INITIALIZER = {0, 0, {0, 0, 0, 0, 0, 0}};

    typedef struct alignas(64) {
        uint64_t lock_bit;
#ifdef __x86_64__
        uint64_t padding[7]; //make sure we take up an entire cache line to minimize bus traffic (https://wiki.osdev.org/Spinlock)
#endif
    } rwlock_t;

    const rwlock_t RWLOCK_INITIALIZER = {0, {0, 0, 0, 0, 0, 0, 0}};
}
#endif //CROCOS_SPINLOCK_H
