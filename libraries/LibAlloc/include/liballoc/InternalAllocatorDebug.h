//
// Created by Spencer Martin on 8/20/25.
//

#ifndef INTERNALALLOCATORDEBUG_H
#define INTERNALALLOCATORDEBUG_H

#include <stddef.h>

namespace LibAlloc::InternalAllocator {
    void validateAllocatorIntegrity();
    size_t computeTotalAllocatedSpace();
    size_t computeTotalFreeSpace();
    bool isValidPointer(void* ptr);
}

#endif //INTERNALALLOCATORDEBUG_H
