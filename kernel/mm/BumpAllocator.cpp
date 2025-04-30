//
// Created by Spencer Martin on 2/15/25.
//

#include <kconfig.h>
#include <kernel.h>
#include "allocators.h"
#include "arch/hal/hal.h"
#include <core/atomic.h>
#include <assert.h>

uint8_t buffer[KERNEL_BUMP_ALLOC_SIZE];
uint64_t free_index = 0;

WITH_GLOBAL_CONSTRUCTOR(Spinlock, lock);

namespace kernel::mm::allocators{
    void* bump_alloc(size_t size, std::align_val_t align){
        assert(free_index + size <= KERNEL_BUMP_ALLOC_SIZE, "Kernel bump allocator full");
        LockGuard guard(lock);
        //probably unnecessary locking, since we should no longer be using the bump allocator by the time we spin
        //up the other cores, but it's better to err on the side of overly cautious, especially for
        size_t misalign = (size_t)&buffer[free_index] % (size_t)align;
        free_index += ((size_t)align - misalign) % (size_t)align;
        auto out = &buffer[free_index];
        free_index += size;
        return out;
    }

    bool in_bump_alloc_range(void* ptr){
        return ptr >= buffer && ptr < (buffer + KERNEL_BUMP_ALLOC_SIZE);
    }
}
