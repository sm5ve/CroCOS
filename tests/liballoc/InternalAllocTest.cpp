//
// Unit tests for Interrupt Graph Infrastructure
// Created by Spencer Martin on 7/27/25.
//

#define CROCOS_TESTING
#include "../test.h"
#include <TestHarness.h>
#include <cstdlib>
#include <liballoc/InternalAllocator.h>
#include <liballoc/InternalAllocatorDebug.h>
#include <core/ds/Vector.h>
#include <chrono>

using namespace CroCOSTest;

TEST(basicMallocFreeTest) {
    void* mem = LibAlloc::InternalAllocator::malloc(100);
    ASSERT_NE(mem, nullptr);
    ASSERT_TRUE(LibAlloc::InternalAllocator::isValidPointer(mem));
    ASSERT_FALSE(LibAlloc::InternalAllocator::isValidPointer(nullptr));
    ASSERT_FALSE(LibAlloc::InternalAllocator::isValidPointer((void*)basicMallocFreeTest));
    void* internalPointer = (void*)((uintptr_t)mem + 10);
    ASSERT_FALSE(LibAlloc::InternalAllocator::isValidPointer(internalPointer));
    LibAlloc::InternalAllocator::free(mem);
    ASSERT_FALSE(LibAlloc::InternalAllocator::isValidPointer(mem));
    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);
}

TEST(randomAllocFreeStressTest) {
    const int maxAllocations = 200;
    const int maxLoops = 20;

    std::srand(42); // Reproducible results

    for (int loop = 0; loop < maxLoops; loop++) {
        Vector<void*> allocatedPointers;
        Vector<size_t> allocationSizes;

        // Phase 1: Random allocations
        for (int i = 0; i < maxAllocations; i++) {
            size_t size = (std::rand() % 2048) + 1; // 1 to 2048 bytes
            void* ptr = LibAlloc::InternalAllocator::malloc(size);

            ASSERT_NE(ptr, nullptr);
            ASSERT_TRUE(LibAlloc::InternalAllocator::isValidPointer(ptr));

            allocatedPointers.push(ptr);
            allocationSizes.push(size);

            // Fill with pattern to detect corruption
            auto* bytes = static_cast<uint8_t*>(ptr);
            for (size_t j = 0; j < size; j++) {
                bytes[j] = static_cast<uint8_t>((i + j) & 0xFF);
            }
        }

        // Verify all allocations are still valid and uncorrupted
        for (size_t i = 0; i < allocatedPointers.getSize(); i++) {
            void* ptr = allocatedPointers[i];
            size_t size = allocationSizes[i];

            ASSERT_TRUE(LibAlloc::InternalAllocator::isValidPointer(ptr));

            // Check pattern
            auto* bytes = static_cast<uint8_t*>(ptr);
            for (size_t j = 0; j < size; j++) {
                ASSERT_EQ(bytes[j], static_cast<uint8_t>((i + j) & 0xFF));
            }
        }

        // Phase 2: Random frees
        while (!allocatedPointers.empty()) {
            int index = std::rand() % allocatedPointers.getSize();
            void* ptr = allocatedPointers[index];

            LibAlloc::InternalAllocator::free(ptr);
            ASSERT_FALSE(LibAlloc::InternalAllocator::isValidPointer(ptr));

            // Remove from tracking
            allocatedPointers[index] = allocatedPointers[-1];
            allocatedPointers.pop();
            allocationSizes[index] = allocationSizes[-1];
            allocationSizes.pop();

            // Validate integrity after each free
            LibAlloc::InternalAllocator::validateAllocatorIntegrity();
        }

        ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);
    }
}

TEST(fragmentationResistanceTest) {
    Vector<void*> smallAllocations;
    Vector<void*> largeAllocations;

    // Create fragmentation pattern: allocate many small blocks
    for (int i = 0; i < 100; i++) {
        void* ptr = LibAlloc::InternalAllocator::malloc(64);
        ASSERT_NE(ptr, nullptr);
        smallAllocations.push(ptr);
    }

    // Free every other small block to create fragmentation
    for (size_t i = 1; i < smallAllocations.getSize(); i += 2) {
        LibAlloc::InternalAllocator::free(smallAllocations[i]);
        smallAllocations[i] = nullptr;
    }

    // Try to allocate larger blocks - should still succeed despite fragmentation
    for (int i = 0; i < 20; i++) {
        void* ptr = LibAlloc::InternalAllocator::malloc(512);
        ASSERT_NE(ptr, nullptr);
        largeAllocations.push(ptr);
    }

    // Clean up
    for (void* ptr : smallAllocations) {
        if (ptr != nullptr) {
            LibAlloc::InternalAllocator::free(ptr);
        }
    }
    for (void* ptr : largeAllocations) {
        LibAlloc::InternalAllocator::free(ptr);
    }

    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);
}

TEST(coalescingStressTest) {
    Vector<void*> allocations;
    const int blockCount = 50;
    const size_t blockSize = 128;

    // Allocate adjacent blocks
    for (int i = 0; i < blockCount; i++) {
        void* ptr = LibAlloc::InternalAllocator::malloc(blockSize);
        ASSERT_NE(ptr, nullptr);
        allocations.push(ptr);
    }

    // Free blocks in various patterns to test coalescing

    // Pattern 1: Free every other block
    for (int i = 1; i < blockCount; i += 2) {
        LibAlloc::InternalAllocator::free(allocations[i]);
        allocations[i] = nullptr;
        LibAlloc::InternalAllocator::validateAllocatorIntegrity();
    }

    // Pattern 2: Free remaining blocks to trigger more coalescing
    for (int i = 0; i < blockCount; i += 2) {
        if (allocations[i] != nullptr) {
            LibAlloc::InternalAllocator::free(allocations[i]);
            allocations[i] = nullptr;
            LibAlloc::InternalAllocator::validateAllocatorIntegrity();
        }
    }

    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);

    // After coalescing, we should be able to allocate a large block
    void* largeBlock = LibAlloc::InternalAllocator::malloc(blockCount * blockSize / 2);
    ASSERT_NE(largeBlock, nullptr);

    LibAlloc::InternalAllocator::free(largeBlock);
    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
}

TEST(alignmentStressTest) {
    Vector<void*> allocations;
    std::srand(123);

    // Test various alignment requirements
    const std::align_val_t alignments[] = {
        std::align_val_t{8}, std::align_val_t{16}, std::align_val_t{32},
        std::align_val_t{64}, std::align_val_t{128}, std::align_val_t{256}
    };

    for (int loop = 0; loop < 300; loop++) {
        std::align_val_t align = alignments[std::rand() % 6];
        size_t size = (std::rand() % 1024) + 1;

        void* ptr = LibAlloc::InternalAllocator::malloc(size, align);
        ASSERT_NE(ptr, nullptr);

        // Verify alignment
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        ASSERT_EQ(addr % static_cast<size_t>(align), 0);

        ASSERT_TRUE(LibAlloc::InternalAllocator::isValidPointer(ptr));
        allocations.push(ptr);
    }

    // Free in random order
    while (!allocations.empty()) {
        int index = std::rand() % allocations.getSize();
        LibAlloc::InternalAllocator::free(allocations[index]);
        allocations[index] = allocations[-1];
        allocations.pop();
        LibAlloc::InternalAllocator::validateAllocatorIntegrity();
    }

    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);
}

TEST(mixedSizeStressTest) {
    Vector<void*> allocations;
    std::srand(456);

    // Mix of tiny, small, medium, and large allocations
    const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    for (int i = 0; i < 150; i++) {
        size_t size = sizes[std::rand() % 10];
        void* ptr = LibAlloc::InternalAllocator::malloc(size);

        ASSERT_NE(ptr, nullptr);
        ASSERT_TRUE(LibAlloc::InternalAllocator::isValidPointer(ptr));
        allocations.push(ptr);

        //Zero out the allocation to confirm doing so doens't corrupt the allocator state
        //This is to make sure the header of one block is not contained in the buffer of another
        memset(ptr, 0, size);

        // Randomly free some allocations to maintain working set
        if (allocations.getSize() > 75 && (std::rand() % 3) == 0) {
            int freeIndex = std::rand() % allocations.getSize();
            LibAlloc::InternalAllocator::free(allocations[freeIndex]);
            allocations[freeIndex] = allocations[-1];
            allocations.pop();
        }

        if (i % 25 == 0) {
            LibAlloc::InternalAllocator::validateAllocatorIntegrity();
        }
    }

    // Clean up remaining allocations
    for (void* ptr : allocations) {
        LibAlloc::InternalAllocator::free(ptr);
    }

    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpaceInCoarseAllocator(), 0);
}

struct AllocationRecord{
    void* ptr;
    size_t size;
};

TEST(allocatorPerformanceStressTest) {
    const int testDurationSeconds = 2;
    const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    Vector<AllocationRecord> allocations;
    std::srand(789); // Different seed for performance test

    // Performance statistics
    size_t totalAllocations = 0;
    size_t totalFrees = 0;
    size_t totalBytesAllocated = 0;
    size_t totalBytesFreed = 0;
    size_t peakActiveAllocations = 0;
    size_t peakActiveBytes = 0;

    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(testDurationSeconds);

    printf("\nRunning allocator performance test for %d seconds...\n", testDurationSeconds);

    // Memory pressure management
    const size_t maxTotalBytes = 50 * 1024 * 1024; // 50MB limit
    const size_t pressureThreshold = maxTotalBytes * 0.8; // Start pressure at 80%

    while (std::chrono::steady_clock::now() < endTime) {
		for(size_t _inner = 0; _inner < 10000; _inner++) {
			// Use fast O(1) statistics instead of expensive tree traversal
        	size_t currentActiveBytes = LibAlloc::InternalAllocator::getAllocatorStats().totalUsedBytesInAllocator;

        	// Calculate allocation probability based on memory pressure
        	bool shouldAllocate;
        	if (allocations.empty()) {
        	    shouldAllocate = true; // Must allocate if nothing is allocated
        	} else if (currentActiveBytes >= maxTotalBytes) {
        	    shouldAllocate = false; // Force free if at limit
        	} else if (currentActiveBytes >= pressureThreshold) {
        	    // Gradually reduce allocation probability as we approach limit
        	    double pressureRatio = (double)(currentActiveBytes - pressureThreshold) / (maxTotalBytes - pressureThreshold);
        	    double allocProbability = 0.7 * (1.0 - pressureRatio); // Scale down from 70%
        	    shouldAllocate = (std::rand() % 100) < (allocProbability * 100);
        	} else {
        	    shouldAllocate = (std::rand() % 10) < 7; // Normal 70% chance
        	}

        	if (shouldAllocate) {
        	    size_t size = sizes[std::rand() % 10];
        	    void* ptr = LibAlloc::InternalAllocator::malloc(size);
        	    ASSERT_NE(ptr, nullptr);

   		        allocations.push({ptr, size});
    	        totalAllocations++;
	            totalBytesAllocated += size;

        	    // Track peaks
    	        if (allocations.getSize() > peakActiveAllocations) {
	                peakActiveAllocations = allocations.getSize();
	            }

        	    // currentActiveBytes already computed above for memory pressure
        	    if (currentActiveBytes > peakActiveBytes) {
        	        peakActiveBytes = currentActiveBytes;
        	    }
        	} else {
            	// Free a random allocation
            	int index = std::rand() % allocations.getSize();
            	void* ptr = allocations[index].ptr;

            	LibAlloc::InternalAllocator::free(ptr);

        	    totalBytesFreed += allocations[index].size;
        	    allocations[index] = allocations[allocations.getSize()-1];
        	    allocations.pop();
        	    totalFrees++;
        	}

        	// Periodic random validation during sustained load
        	if (std::rand() % 50000 == 0) {
        	    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
        	}
		}
    }

    auto actualEndTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(actualEndTime - startTime);
    double durationSeconds = duration.count() / 1000.0;

    // Clean up remaining allocations
    for (auto info : allocations) {
        LibAlloc::InternalAllocator::free(info.ptr);
        totalFrees++;
        totalBytesFreed += info.size;
    }

    // Get final statistics
    auto finalStats = LibAlloc::InternalAllocator::getAllocatorStats();

    // Final validation
    LibAlloc::InternalAllocator::validateAllocatorIntegrity();
	printf("\nRemaining bytes in allocator: %zu\n", finalStats.totalUsedBytesInAllocator);
    ASSERT_EQ(finalStats.totalUsedBytesInAllocator, 0);

    // Print performance statistics
    printf("\n=== Allocator Performance Statistics ===\n");
    printf("Test Duration: %.3f seconds\n", durationSeconds);
    printf("Total Operations: %zu\n", totalAllocations + totalFrees);
    printf("Operations/second: %.0f\n", (totalAllocations + totalFrees) / durationSeconds);
    printf("\nAllocation Stats:\n");
    printf("  Total Allocations: %zu\n", totalAllocations);
    printf("  Allocations/second: %.0f\n", totalAllocations / durationSeconds);
    printf("  Total Bytes Allocated: %zu (%.2f MB)\n", totalBytesAllocated, totalBytesAllocated / (1024.0 * 1024.0));
    printf("  Allocation Throughput: %.2f MB/s\n", (totalBytesAllocated / (1024.0 * 1024.0)) / durationSeconds);
    printf("\nFree Stats:\n");
    printf("  Total Frees: %zu\n", totalFrees);
    printf("  Frees/second: %.0f\n", totalFrees / durationSeconds);
    printf("  Total Bytes Freed: %zu (%.2f MB)\n", totalBytesFreed, totalBytesFreed / (1024.0 * 1024.0));
    printf("\nPeak Usage:\n");
    printf("  Peak Active Allocations: %zu\n", peakActiveAllocations);
    printf("  Peak Active Memory: %zu bytes (%.2f MB)\n", peakActiveBytes, peakActiveBytes / (1024.0 * 1024.0));
    printf("  Average Allocation Size: %.1f bytes\n", totalBytesAllocated / (double)totalAllocations);
    printf("\nAllocator Efficiency:\n");
    printf("  Peak System Memory: %zu bytes (%.2f MB)\n", finalStats.totalSystemMemoryAllocated, finalStats.totalSystemMemoryAllocated / (1024.0 * 1024.0));
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
    printf("  Total Bytes Requested: %zu (%.2f MB)\n", finalStats.totalBytesRequested, finalStats.totalBytesRequested / (1024.0 * 1024.0));
#endif
    printf("==========================================\n\n");
}

/*TEST(nativeAllocatorPerformanceComparison) {
    const int testDurationSeconds = 2;
    const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    Vector<AllocationRecord> allocations;
    std::srand(789); // Different seed for performance test

    // Performance statistics
    size_t totalAllocations = 0;
    size_t totalFrees = 0;
    size_t totalBytesAllocated = 0;
    size_t totalBytesFreed = 0;
    size_t peakActiveAllocations = 0;
    size_t peakActiveBytes = 0;

    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(testDurationSeconds);

    printf("\nRunning allocator performance test for %d seconds...\n", testDurationSeconds);

    // Memory pressure management
    const size_t maxTotalBytes = 50 * 1024 * 1024; // 50MB limit
    const size_t pressureThreshold = maxTotalBytes * 0.8; // Start pressure at 80%
    size_t currentActiveBytes = 0;

    while (std::chrono::steady_clock::now() < endTime) {
        // Use fast O(1) statistics instead of expensive tree traversal

        // Calculate allocation probability based on memory pressure
        bool shouldAllocate;
        if (allocations.empty()) {
            shouldAllocate = true; // Must allocate if nothing is allocated
        } else if (currentActiveBytes >= maxTotalBytes) {
            shouldAllocate = false; // Force free if at limit
        } else if (currentActiveBytes >= pressureThreshold) {
            // Gradually reduce allocation probability as we approach limit
            double pressureRatio = (double)(currentActiveBytes - pressureThreshold) / (maxTotalBytes - pressureThreshold);
            double allocProbability = 0.7 * (1.0 - pressureRatio); // Scale down from 70%
            shouldAllocate = (std::rand() % 100) < (allocProbability * 100);
        } else {
            shouldAllocate = (std::rand() % 10) < 7; // Normal 70% chance
        }

        if (shouldAllocate) {
            size_t size = sizes[std::rand() % 10];
            currentActiveBytes += size;
            void* ptr = malloc(size);
            //ASSERT_NE(ptr, nullptr);

            allocations.push({ptr, size});
            totalAllocations++;
            totalBytesAllocated += size;

            // Track peaks
            if (allocations.getSize() > peakActiveAllocations) {
                peakActiveAllocations = allocations.getSize();
            }

            // currentActiveBytes already computed above for memory pressure
            if (currentActiveBytes > peakActiveBytes) {
                peakActiveBytes = currentActiveBytes;
            }
        } else {
            // Free a random allocation
            int index = std::rand() % allocations.getSize();
            void* ptr = allocations[index].ptr;

            free(ptr);

            currentActiveBytes -= allocations[index].size;
            totalBytesFreed += allocations[index].size;
            allocations[index] = allocations[allocations.getSize()-1];
            allocations.pop();
            totalFrees++;
        }
    }

    auto actualEndTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(actualEndTime - startTime);
    double durationSeconds = duration.count() / 1000.0;

    // Clean up remaining allocations
    for (auto info : allocations) {
        free(info.ptr);
        totalFrees++;
        totalBytesFreed += info.size;
    }

    // Print performance statistics
    printf("\n=== Allocator Performance Statistics ===\n");
    printf("Test Duration: %.3f seconds\n", durationSeconds);
    printf("Total Operations: %zu\n", totalAllocations + totalFrees);
    printf("Operations/second: %.0f\n", (totalAllocations + totalFrees) / durationSeconds);
    printf("\nAllocation Stats:\n");
    printf("  Total Allocations: %zu\n", totalAllocations);
    printf("  Allocations/second: %.0f\n", totalAllocations / durationSeconds);
    printf("  Total Bytes Allocated: %zu (%.2f MB)\n", totalBytesAllocated, totalBytesAllocated / (1024.0 * 1024.0));
    printf("  Allocation Throughput: %.2f MB/s\n", (totalBytesAllocated / (1024.0 * 1024.0)) / durationSeconds);
    printf("\nFree Stats:\n");
    printf("  Total Frees: %zu\n", totalFrees);
    printf("  Frees/second: %.0f\n", totalFrees / durationSeconds);
    printf("  Total Bytes Freed: %zu (%.2f MB)\n", totalBytesFreed, totalBytesFreed / (1024.0 * 1024.0));
    printf("\nPeak Usage:\n");
    printf("  Peak Active Allocations: %zu\n", peakActiveAllocations);
    printf("  Peak Active Memory: %zu bytes (%.2f MB)\n", peakActiveBytes, peakActiveBytes / (1024.0 * 1024.0));
    printf("  Average Allocation Size: %.1f bytes\n", totalBytesAllocated / (double)totalAllocations);
    printf("==========================================\n\n");
}*/