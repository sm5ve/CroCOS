//
// Created by Spencer Martin on 2/12/26.
//

#include "../test.h"
#include <arch.h>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace arch {
    namespace {
        // Thread ID to ProcessorID mapping for multithreaded tests
        std::mutex processorIdMapMutex;
        std::unordered_map<std::thread::id, ProcessorID> threadToProcessorId;
        std::atomic<ProcessorID> nextProcessorId{0};

        // Configurable processor count for testing (default: 8)
        size_t mockProcessorCount = 8;

        // Thread-local cache to avoid lock contention on hot path
        thread_local ProcessorID cachedProcessorId = 255; // Invalid ID initially
        thread_local bool processorIdCached = false;
    }

    // Test-only functions for resetting state between tests
    namespace testing {
        void resetProcessorState() {
            std::lock_guard<std::mutex> lock(processorIdMapMutex);
            threadToProcessorId.clear();
            nextProcessorId.store(0);
            // Note: thread_local variables persist, but will be reassigned on next access
        }

        void setProcessorCount(size_t count) {
            std::lock_guard<std::mutex> lock(processorIdMapMutex);
            mockProcessorCount = count;
        }

        size_t getProcessorCount() {
            std::lock_guard<std::mutex> lock(processorIdMapMutex);
            return mockProcessorCount;
        }
    }

    ProcessorID getCurrentProcessorID() {
        // Fast path: return cached ID if already assigned
        if (processorIdCached) {
            return cachedProcessorId;
        }

        // Slow path: assign a processor ID to this thread
        std::thread::id tid = std::this_thread::get_id();

        {
            std::lock_guard<std::mutex> lock(processorIdMapMutex);

            // Check if this thread already has an ID (might have been assigned by another call)
            auto it = threadToProcessorId.find(tid);
            if (it != threadToProcessorId.end()) {
                cachedProcessorId = it->second;
                processorIdCached = true;
                return cachedProcessorId;
            }

            // Assign a new processor ID (round-robin across available processors)
            ProcessorID pid = nextProcessorId.fetch_add(1) % mockProcessorCount;
            threadToProcessorId[tid] = pid;

            cachedProcessorId = pid;
            processorIdCached = true;
            return pid;
        }
    }

    size_t processorCount() {
        // No lock needed - reading size_t is atomic on modern architectures
        return mockProcessorCount;
    }
}
