//
// Created by Spencer Martin on 8/1/25.
//

#ifndef INTERNALALLOCATOR_H
#define INTERNALALLOCATOR_H

#include <stddef.h>

namespace LibAlloc::InternalAllocator {
	void initializeInternalAllocator();

    void* malloc(size_t size, std::align_val_t align = std::align_val_t(alignof(uint64_t)));

    void free(void* ptr);
}

#endif //INTERNALALLOCATOR_H
