//
// Memory tracking instrumentation implementation
// Created by Spencer Martin on 7/24/25.
//

#include "MemoryTracker.h"
#include <iostream>
#include <cstdlib>
#include <new>
#include "MemoryTrackingGuard.h"  // For globally constructed objects invoking the memory allocator

// Define the garbage pointer used by placeholder implementations in test.h
volatile void* __garbage = nullptr;

namespace CroCOSTest {

    // Helper function to get the tracker mutex as a function-local static
    // This ensures the mutex is never destroyed, avoiding issues during static destruction
    static std::mutex& getTrackerMutex() {
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    // Static member definitions
    std::unordered_map<void*, AllocationInfo> MemoryTracker::allocations;
    size_t MemoryTracker::total_allocated = 0;
    size_t MemoryTracker::total_freed = 0;
    size_t MemoryTracker::peak_usage = 0;
    size_t MemoryTracker::current_usage = 0;
    bool track = true;

    void pauseTracking() {
        track = false;
    }

    void resumeTracking() {
        track = true;
    }

    bool getTrackingStatus() {
        return track;
    }

    void MemoryTracker::recordAllocation(void* ptr, size_t size) {
        if (!ptr) return;
        if (!track) return;

        std::lock_guard<std::mutex> lock(getTrackerMutex());
        allocations[ptr] = {size};
        total_allocated += size;
        current_usage += size;
        if (current_usage > peak_usage) {
            peak_usage = current_usage;
        }
    }

    void MemoryTracker::recordDeallocation(void* ptr) {
        if (!ptr) return;
        if (!track) return;

        std::lock_guard<std::mutex> lock(getTrackerMutex());
        auto it = allocations.find(ptr);
        if (it != allocations.end()) {
            total_freed += it->second.size;
            current_usage -= it->second.size;
            allocations.erase(it);
        }
    }

    bool MemoryTracker::hasLeaks() {
        std::lock_guard<std::mutex> lock(getTrackerMutex());
        return !allocations.empty();
    }

    void MemoryTracker::printLeakReport() {
        std::lock_guard<std::mutex> lock(getTrackerMutex());
        
        std::cout << "\n=== Memory Leak Report ===" << std::endl;
        std::cout << "Total allocated: " << total_allocated << " bytes" << std::endl;
        std::cout << "Total freed: " << total_freed << " bytes" << std::endl;
        std::cout << "Peak usage: " << peak_usage << " bytes" << std::endl;
        std::cout << "Current usage: " << current_usage << " bytes" << std::endl;
        std::cout << "Active allocations: " << allocations.size() << std::endl;
        
        if (!allocations.empty()) {
            std::cout << "\nLEAKED ALLOCATIONS:" << std::endl;
            for (const auto& [ptr, info] : allocations) {
                std::cout << "  " << ptr << " -> " << info.size << " bytes" << std::endl;
            }
        } else {
            std::cout << "\nNo memory leaks detected!" << std::endl;
        }
        std::cout << "=========================" << std::endl;
    }

    void MemoryTracker::reset() {
        std::lock_guard<std::mutex> lock(getTrackerMutex());
        allocations.clear();
        total_allocated = 0;
        total_freed = 0;
        peak_usage = 0;
        current_usage = 0;
    }
}

// Tracked allocation functions that objcopy will redirect Core library calls to
// These replace malloc/free calls only within the instrumented CoreTest library

extern "C" {

void* _tracked_malloc(size_t size) {
    void* ptr = std::malloc(size);
    CroCOSTest::MemoryTracker::recordAllocation(ptr, size);
    return ptr;
}

void* __tracked_malloc(size_t size) {
    return _tracked_malloc(size);
}

void* _tracked_calloc(size_t count, size_t size) {
    void* ptr = std::calloc(count, size);
    CroCOSTest::MemoryTracker::recordAllocation(ptr, count * size);
    return ptr;
}

void* __tracked_calloc(size_t count, size_t size) {
    return _tracked_calloc(count, size);
}

void* _tracked_realloc(void* old_ptr, size_t size) {
    if (old_ptr) {
        CroCOSTest::MemoryTracker::recordDeallocation(old_ptr);
    }
    void* ptr = std::realloc(old_ptr, size);
    if (ptr && size > 0) {
        CroCOSTest::MemoryTracker::recordAllocation(ptr, size);
    }
    return ptr;
}

void* __tracked_realloc(void* old_ptr, size_t size) {
    return _tracked_realloc(old_ptr, size);
}

void _tracked_free(void* ptr) {
    CroCOSTest::MemoryTracker::recordDeallocation(ptr);
    std::free(ptr);
}

void __tracked_free(void* ptr) {
    _tracked_free(ptr);
}

// Tracked C++ new/delete operators
void* _tracked_new(size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    CroCOSTest::MemoryTracker::recordAllocation(ptr, size);
    return ptr;
}

void* __tracked_new(size_t size) {
    return _tracked_new(size);
}

void* _tracked_new_array(size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    CroCOSTest::MemoryTracker::recordAllocation(ptr, size);
    return ptr;
}

void* __tracked_new_array(size_t size) {
    return _tracked_new_array(size);
}

void _tracked_delete(void* ptr) {
    if (ptr) {
        CroCOSTest::MemoryTracker::recordDeallocation(ptr);
        std::free(ptr);
    }
}

void __tracked_delete(void* ptr) {
    _tracked_delete(ptr);
}

void _tracked_delete_array(void* ptr) {
    if (ptr) {
        CroCOSTest::MemoryTracker::recordDeallocation(ptr);
        std::free(ptr);
    }
}

void __tracked_delete_array(void* ptr) {
    _tracked_delete_array(ptr);
}

void _tracked_delete_sized(void* ptr, size_t) {
    if (ptr) {
        CroCOSTest::MemoryTracker::recordDeallocation(ptr);
        std::free(ptr);
    }
}

void __tracked_delete_sized(void* ptr, size_t size) {
    _tracked_delete_sized(ptr, size);
}

void _tracked_delete_array_sized(void* ptr, size_t) {
    if (ptr) {
        CroCOSTest::MemoryTracker::recordDeallocation(ptr);
        std::free(ptr);
    }
}

void __tracked_delete_array_sized(void* ptr, size_t size) {
    _tracked_delete_array_sized(ptr, size);
}

} //extern "C"

MemoryTrackingGuard::MemoryTrackingGuard() {
    this -> initialStatus = CroCOSTest::getTrackingStatus();
    CroCOSTest::pauseTracking();
}

MemoryTrackingGuard::~MemoryTrackingGuard() {
    if(this -> initialStatus) {
        CroCOSTest::resumeTracking();
    }
}