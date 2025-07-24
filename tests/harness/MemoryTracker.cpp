//
// Memory tracking instrumentation implementation
// Created by Spencer Martin on 7/24/25.
//

#include "MemoryTracker.h"
#include <iostream>
#include <cstdlib>
#include <new>

// Define the garbage pointer used by placeholder implementations in test.h
volatile void* __garbage = nullptr;

namespace CroCOSTest {

    // Static member definitions
    std::unordered_map<void*, AllocationInfo> MemoryTracker::allocations;
    std::mutex MemoryTracker::tracker_mutex;
    size_t MemoryTracker::total_allocated = 0;
    size_t MemoryTracker::total_freed = 0;
    size_t MemoryTracker::peak_usage = 0;
    size_t MemoryTracker::current_usage = 0;

    void MemoryTracker::recordAllocation(void* ptr, size_t size) {
        if (!ptr) return;
        
        std::lock_guard<std::mutex> lock(tracker_mutex);
        allocations[ptr] = {size};
        total_allocated += size;
        current_usage += size;
        if (current_usage > peak_usage) {
            peak_usage = current_usage;
        }
    }

    void MemoryTracker::recordDeallocation(void* ptr) {
        if (!ptr) return;
        
        std::lock_guard<std::mutex> lock(tracker_mutex);
        auto it = allocations.find(ptr);
        if (it != allocations.end()) {
            total_freed += it->second.size;
            current_usage -= it->second.size;
            allocations.erase(it);
        }
    }

    bool MemoryTracker::hasLeaks() {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        return !allocations.empty();
    }

    void MemoryTracker::printLeakReport() {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        
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
        std::lock_guard<std::mutex> lock(tracker_mutex);
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