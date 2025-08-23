//
// Created by Spencer Martin on 8/20/25.
//

#ifndef INTERNALALLOCATORDEBUG_H
#define INTERNALALLOCATORDEBUG_H

#include <stddef.h>

#define TRACK_REQUESTED_ALLOCATION_STATS

namespace LibAlloc::InternalAllocator {
    struct InternalAllocatorStats {
        size_t totalSystemMemoryAllocated;
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        size_t totalBytesRequested;
#endif
        size_t totalUsedBytesInAllocator;

#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        size_t computeAllocatorMetadataOverhead() const;
        //float computeAllocatorMetadataPercentOverhead() const;
#endif
    };

    void validateAllocatorIntegrity();
    size_t computeTotalAllocatedSpaceInCoarseAllocator();
    size_t computeTotalFreeSpaceInCoarseAllocator();
    bool isValidPointer(void* ptr);
    InternalAllocatorStats getAllocatorStats();
    size_t getInternalAllocRemainingSlabCount();
}

#endif //INTERNALALLOCATORDEBUG_H
