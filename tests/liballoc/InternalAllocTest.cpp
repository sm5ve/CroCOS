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
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
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
        
        ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
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
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
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
    
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
    
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
    
    for (int loop = 0; loop < 30; loop++) {
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
    
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
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
    ASSERT_EQ(LibAlloc::InternalAllocator::computeTotalAllocatedSpace(), 0);
}