//
// Memory tracking instrumentation for CroCOS unit tests
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_MEMORYTRACKER_H
#define CROCOS_MEMORYTRACKER_H

#include <cstddef>
#include <unordered_map>
#include <mutex>

namespace CroCOSTest {

    struct AllocationInfo {
        size_t size;
        // Future: could add stack trace or debug info here
    };

    // Forward declarations for tracking control
    void pauseTracking();
    void resumeTracking();
    bool getTrackingStatus();

    class MemoryTracker {
    private:
        static std::unordered_map<void*, AllocationInfo> allocations;
        static size_t total_allocated;
        static size_t total_freed;
        static size_t peak_usage;
        static size_t current_usage;

    public:
        static void recordAllocation(void* ptr, size_t size);
        static void recordDeallocation(void* ptr);
        static bool hasLeaks();
        static void printLeakReport();
        static void reset();
        static size_t getCurrentUsage() { return current_usage; }
        static size_t getPeakUsage() { return peak_usage; }
        static size_t getTotalAllocated() { return total_allocated; }
        static size_t getTotalFreed() { return total_freed; }
        static size_t getActiveAllocationCount() { return allocations.size(); }
    };
}

#endif //CROCOS_MEMORYTRACKER_H