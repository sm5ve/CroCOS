//
// Unit tests for Page Allocator components
// Created by Spencer Martin on 2/12/26.
//

#include "../test.h"
#include <TestHarness.h>

#include <mem/PageAllocator.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <set>
#include "ArchMocks.h"

using namespace CroCOSTest;

// ============================================================================
// Test Setup/Teardown for Processor State
// ============================================================================

class PageAllocatorTestSetup {
public:
    PageAllocatorTestSetup() {
        arch::testing::resetProcessorState();
        arch::testing::setProcessorCount(8);
    }

    ~PageAllocatorTestSetup() {
        arch::testing::resetProcessorState();
    }
};

// ============================================================================
// PressureBitmap Basic Tests
// ============================================================================

TEST(PressureBitmapMeasureAllocation) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);

    size_t bytesNeeded = measureAllocator.bytesNeeded();

    // Should allocate 4 bitmaps (one per pressure level)
    // Each bitmap needs ceil((8+1)/64) = 1 word of 8 bytes
    // Total: 4 * 1 * 8 = 32 bytes
    ASSERT_EQ(32u, bytesNeeded);
}

TEST(PressureBitmapMeasureAllocationLarge) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 100);

    size_t bytesNeeded = measureAllocator.bytesNeeded();

    // Should allocate 4 bitmaps
    // Each bitmap needs ceil((100+1)/64) = 2 words of 8 bytes
    // Total: 4 * 2 * 8 = 64 bytes
    ASSERT_EQ(64u, bytesNeeded);
}

TEST(PressureBitmapConstruction) {
    PageAllocatorTestSetup setup;

    // Measure first
    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    size_t bytesNeeded = measureAllocator.bytesNeeded();

    // Allocate buffer
    void* buffer = malloc(bytesNeeded);
    ASSERT_TRUE(buffer != nullptr);

    // Create actual allocator
    BootstrapAllocator allocator(buffer, bytesNeeded);
    PressureBitmap bitmap(allocator, 8);

    // Should construct without error
    // All pools should start unmarked (no pressure set)

    free(buffer);
}

// ============================================================================
// PressureBitmap Marking Tests
// ============================================================================

TEST(PressureBitmapMarkSinglePoolSurplus) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark pool 0 as SURPLUS
    PoolID pool0(static_cast<arch::ProcessorID>(0));
    bitmap.markPressure(pool0, PoolPressure::SURPLUS);

    // Check that pool 0 appears in SURPLUS pressure
    auto surplusPools = bitmap.poolsWithPressure(PoolPressure::SURPLUS);
    auto it = surplusPools.begin();
    ASSERT_FALSE(it.atEnd());
    ASSERT_EQ(pool0.id, (*it).id);

    ++it;
    ASSERT_TRUE(it.atEnd());

    // Pool 0 should not appear in other pressure levels
    auto comfortablePools = bitmap.poolsWithPressure(PoolPressure::COMFORTABLE);
    ASSERT_TRUE(comfortablePools.begin().atEnd());

    free(buffer);
}

TEST(PressureBitmapMarkMultiplePoolsSamePressure) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark pools 0, 2, 4 as MODERATE
    PoolID pool0(static_cast<arch::ProcessorID>(0));
    PoolID pool2(static_cast<arch::ProcessorID>(2));
    PoolID pool4(static_cast<arch::ProcessorID>(4));

    bitmap.markPressure(pool0, PoolPressure::MODERATE);
    bitmap.markPressure(pool2, PoolPressure::MODERATE);
    bitmap.markPressure(pool4, PoolPressure::MODERATE);

    // Collect all pools with MODERATE pressure
    std::vector<PoolID::IDType> moderatePools;
    for (auto pool : bitmap.poolsWithPressure(PoolPressure::MODERATE)) {
        moderatePools.push_back(pool.id);
    }

    // Should have exactly 3 pools
    ASSERT_EQ(3u, moderatePools.size());

    // Verify they're the right ones (in sorted order)
    std::sort(moderatePools.begin(), moderatePools.end());
    ASSERT_EQ(0u, moderatePools[0]);
    ASSERT_EQ(2u, moderatePools[1]);
    ASSERT_EQ(4u, moderatePools[2]);

    free(buffer);
}

TEST(PressureBitmapChangePressureLevel) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    PoolID pool1(static_cast<arch::ProcessorID>(1));

    // Initially mark as SURPLUS
    bitmap.markPressure(pool1, PoolPressure::SURPLUS);

    auto surplusPools = bitmap.poolsWithPressure(PoolPressure::SURPLUS);
    ASSERT_FALSE(surplusPools.begin().atEnd());

    // Change to DESPERATE
    bitmap.markPressure(pool1, PoolPressure::DESPERATE);

    // Should no longer appear in SURPLUS
    auto surplusPools2 = bitmap.poolsWithPressure(PoolPressure::SURPLUS);
    ASSERT_TRUE(surplusPools2.begin().atEnd());

    // Should appear in DESPERATE
    auto desperatePools = bitmap.poolsWithPressure(PoolPressure::DESPERATE);
    auto it = desperatePools.begin();
    ASSERT_FALSE(it.atEnd());
    ASSERT_EQ(pool1.id, (*it).id);

    free(buffer);
}

TEST(PressureBitmapMarkGlobalPool) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark global pool as COMFORTABLE
    bitmap.markPressure(GLOBAL, PoolPressure::COMFORTABLE);

    // Check that global pool appears in COMFORTABLE pressure
    auto comfortablePools = bitmap.poolsWithPressure(PoolPressure::COMFORTABLE);
    auto it = comfortablePools.begin();
    ASSERT_FALSE(it.atEnd());

    PoolID found = *it;
    ASSERT_TRUE(found.global());

    ++it;
    ASSERT_TRUE(it.atEnd());

    free(buffer);
}

TEST(PressureBitmapMixedPressureLevels) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Set up mixed pressure levels
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(0)), PoolPressure::SURPLUS);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(1)), PoolPressure::SURPLUS);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(2)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(3)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(4)), PoolPressure::MODERATE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(5)), PoolPressure::DESPERATE);
    bitmap.markPressure(GLOBAL, PoolPressure::COMFORTABLE);

    // Count pools at each pressure level
    size_t surplusCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::SURPLUS)) {
        (void)_;
        surplusCount++;
    }
    ASSERT_EQ(2u, surplusCount);

    size_t comfortableCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::COMFORTABLE)) {
        (void)_;
        comfortableCount++;
    }
    ASSERT_EQ(3u, comfortableCount);

    size_t moderateCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::MODERATE)) {
        (void)_;
        moderateCount++;
    }
    ASSERT_EQ(1u, moderateCount);

    size_t desperateCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::DESPERATE)) {
        (void)_;
        desperateCount++;
    }
    ASSERT_EQ(1u, desperateCount);

    free(buffer);
}

// ============================================================================
// PressureBitmap Iterator Tests
// ============================================================================

TEST(PressureBitmapIteratorEmpty) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Don't mark any pools
    // All iterators should be empty
    for (size_t i = 0; i < static_cast<size_t>(PoolPressure::COUNT); i++) {
        auto pools = bitmap.poolsWithPressure(static_cast<PoolPressure>(i));
        ASSERT_TRUE(pools.begin().atEnd());
        ASSERT_EQ(pools.begin(), pools.end());
    }

    free(buffer);
}

TEST(PressureBitmapIteratorIncrement) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark several non-consecutive pools
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(1)), PoolPressure::SURPLUS);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(3)), PoolPressure::SURPLUS);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(7)), PoolPressure::SURPLUS);

    auto pools = bitmap.poolsWithPressure(PoolPressure::SURPLUS);
    auto it = pools.begin();

    // Should iterate through in order
    ASSERT_FALSE(it.atEnd());
    ASSERT_EQ(1u, (*it).id);

    ++it;
    ASSERT_FALSE(it.atEnd());
    ASSERT_EQ(3u, (*it).id);

    ++it;
    ASSERT_FALSE(it.atEnd());
    ASSERT_EQ(7u, (*it).id);

    ++it;
    ASSERT_TRUE(it.atEnd());

    free(buffer);
}

TEST(PressureBitmapIteratorAllPools) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark all pools (8 CPUs + 1 global) as DESPERATE
    for (size_t i = 0; i < 8; i++) {
        bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(i)), PoolPressure::DESPERATE);
    }
    bitmap.markPressure(GLOBAL, PoolPressure::DESPERATE);

    // Count them
    size_t count = 0;
    for (auto pool : bitmap.poolsWithPressure(PoolPressure::DESPERATE)) {
        (void)pool;
        count++;
    }

    ASSERT_EQ(9u, count);

    free(buffer);
}

TEST(PressureBitmapIteratorRangeBasedLoop) {
    PageAllocatorTestSetup setup;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Mark pools 0, 1, 2
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(0)), PoolPressure::MODERATE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(1)), PoolPressure::MODERATE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(2)), PoolPressure::MODERATE);

    std::vector<PoolID::IDType> foundPools;
    for (auto pool : bitmap.poolsWithPressure(PoolPressure::MODERATE)) {
        foundPools.push_back(pool.id);
    }

    ASSERT_EQ(3u, foundPools.size());
    ASSERT_EQ(0u, foundPools[0]);
    ASSERT_EQ(1u, foundPools[1]);
    ASSERT_EQ(2u, foundPools[2]);

    free(buffer);
}

// ============================================================================
// PressureBitmap Concurrent Tests
// ============================================================================

TEST(PressureBitmapConcurrentMarking) {
    PageAllocatorTestSetup setup;
    arch::testing::setProcessorCount(8);

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Pause memory tracking during thread creation (std::thread allocates internal state)
    pauseTracking();

    std::atomic<bool> stop{false};
    std::atomic<size_t> totalOperations{0};

    // Launch 8 threads, each continuously marking its own pool for a period
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; i++) {
        threads.emplace_back([&bitmap, &stop, &totalOperations, i]() {
            PoolID pool(static_cast<arch::ProcessorID>(i));
            size_t localOps = 0;

            // Run for many iterations to stress test
            for (size_t iter = 0; iter < 1000 && !stop.load(); iter++) {
                // Cycle through pressure levels
                bitmap.markPressure(pool, PoolPressure::SURPLUS);
                localOps++;
                bitmap.markPressure(pool, PoolPressure::COMFORTABLE);
                localOps++;
                bitmap.markPressure(pool, PoolPressure::MODERATE);
                localOps++;
                bitmap.markPressure(pool, PoolPressure::DESPERATE);
                localOps++;
            }
            totalOperations.fetch_add(localOps);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    resumeTracking();

    // Verify that all threads did work
    ASSERT_TRUE(totalOperations.load() > 0);

    // All pools should end up in DESPERATE (last state set)
    size_t desperateCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::DESPERATE)) {
        (void)_;
        desperateCount++;
    }
    ASSERT_EQ(8u, desperateCount);

    // Other pressure levels should be empty
    size_t otherCount = 0;
    for (auto _ : bitmap.poolsWithPressure(PoolPressure::SURPLUS)) {
        (void)_;
        otherCount++;
    }
    ASSERT_EQ(0u, otherCount);

    free(buffer);
}

TEST(PressureBitmapConcurrentReadWrite) {
    PageAllocatorTestSetup setup;
    arch::testing::setProcessorCount(8);

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, 8);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, 8);

    // Pre-mark some pools
    for (size_t i = 0; i < 4; i++) {
        bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(i)), PoolPressure::MODERATE);
    }

    // Pause memory tracking during thread creation
    pauseTracking();

    std::atomic<bool> stop{false};
    std::atomic<size_t> totalIterations{0};
    std::atomic<size_t> totalPoolsSeen{0};

    // Writer thread that continuously changes pressure levels
    std::thread writer([&]() {
        for (size_t iteration = 0; iteration < 500; iteration++) {
            for (size_t i = 0; i < 8; i++) {
                PoolPressure pressure = static_cast<PoolPressure>(iteration % static_cast<size_t>(PoolPressure::COUNT));
                bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(i)), pressure);
            }
        }
        stop.store(true);
    });

    // Reader threads that continuously iterate
    std::vector<std::thread> readers;
    for (size_t i = 0; i < 4; i++) {
        readers.emplace_back([&]() {
            size_t localIterations = 0;
            size_t localPoolsSeen = 0;

            while (!stop.load()) {
                // Iterate through all pressure levels
                for (size_t p = 0; p < static_cast<size_t>(PoolPressure::COUNT); p++) {
                    size_t poolCount = 0;
                    for (auto pool : bitmap.poolsWithPressure(static_cast<PoolPressure>(p))) {
                        (void)pool;
                        poolCount++;
                        localPoolsSeen++;

                        // Verify pool ID is in valid range
                        ASSERT_TRUE(pool.global() || pool.id < 8);
                    }
                    // Verify we don't see more pools than exist
                    ASSERT_TRUE(poolCount <= 9); // 8 processors + 1 global
                }
                localIterations++;
            }

            totalIterations.fetch_add(localIterations);
            totalPoolsSeen.fetch_add(localPoolsSeen);
        });
    }

    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    resumeTracking();

    // Verify that readers did substantial work without stalling
    ASSERT_TRUE(totalIterations.load() > 0);
    ASSERT_TRUE(totalPoolsSeen.load() > 0);

    // Verify final state is consistent (all pools at same pressure level)
    size_t totalFound = 0;
    for (size_t p = 0; p < static_cast<size_t>(PoolPressure::COUNT); p++) {
        size_t count = 0;
        for (auto _ : bitmap.poolsWithPressure(static_cast<PoolPressure>(p))) {
            (void)_;
            count++;
        }
        totalFound += count;
    }
    // Should find exactly 8 pools (global pool not marked in this test)
    ASSERT_EQ(8u, totalFound);

    free(buffer);
}

// ============================================================================
// PressureBitmap Large Processor Count Tests
// ============================================================================

TEST(PressureBitmapLargeProcessorCount) {
    PageAllocatorTestSetup setup;
    const size_t largeCount = 128;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, largeCount);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, largeCount);

    // Mark every 8th pool as SURPLUS
    for (size_t i = 0; i < largeCount; i += 8) {
        bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(i)), PoolPressure::SURPLUS);
    }

    // Count them
    size_t count = 0;
    std::vector<PoolID::IDType> foundPools;
    for (auto pool : bitmap.poolsWithPressure(PoolPressure::SURPLUS)) {
        foundPools.push_back(pool.id);
        count++;
    }

    ASSERT_EQ(16u, count);

    // Verify they're correct
    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(i * 8, foundPools[i]);
    }

    free(buffer);
}

TEST(PressureBitmapMultipleWordSpan) {
    PageAllocatorTestSetup setup;
    const size_t largeCount = 200;

    BootstrapAllocator measureAllocator;
    PressureBitmap::measureAllocation(measureAllocator, largeCount);
    void* buffer = malloc(measureAllocator.bytesNeeded());

    BootstrapAllocator allocator(buffer, measureAllocator.bytesNeeded());
    PressureBitmap bitmap(allocator, largeCount);

    // Mark pools across word boundaries (64, 65, 66 span two words)
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(63)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(64)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(65)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(128)), PoolPressure::COMFORTABLE);
    bitmap.markPressure(PoolID(static_cast<arch::ProcessorID>(129)), PoolPressure::COMFORTABLE);

    std::vector<PoolID::IDType> foundPools;
    for (auto pool : bitmap.poolsWithPressure(PoolPressure::COMFORTABLE)) {
        foundPools.push_back(pool.id);
    }

    ASSERT_EQ(5u, foundPools.size());
    ASSERT_EQ(63u, foundPools[0]);
    ASSERT_EQ(64u, foundPools[1]);
    ASSERT_EQ(65u, foundPools[2]);
    ASSERT_EQ(128u, foundPools[3]);
    ASSERT_EQ(129u, foundPools[4]);

    free(buffer);
}

// ============================================================================
// SmallPageAllocator Tests
// ============================================================================

// Helper to create buffers for SmallPageAllocator
struct SmallPageAllocatorBuffers {
    static constexpr size_t SMALL_PAGES_PER_BIG_PAGE = 512; // 2MB / 4KB
    SmallPageIndex fwb[SMALL_PAGES_PER_BIG_PAGE];
    SmallPageIndex bwb[SMALL_PAGES_PER_BIG_PAGE];
};

TEST(SmallPageAllocatorConstruction) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Initially all pages should be free
    ASSERT_TRUE(allocator.allFree());
    ASSERT_FALSE(allocator.allFull());
    ASSERT_EQ(512u, allocator.freePageCount());
}

TEST(SmallPageAllocatorAllocateSingle) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate one page
    SmallPageIndex page = allocator.allocateSmallPage();

    ASSERT_FALSE(allocator.allFree());
    ASSERT_FALSE(allocator.allFull());
    ASSERT_EQ(511u, allocator.freePageCount());
}

TEST(SmallPageAllocatorAllocateMultiple) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate 10 pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 10; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }

    ASSERT_FALSE(allocator.allFree());
    ASSERT_FALSE(allocator.allFull());
    ASSERT_EQ(502u, allocator.freePageCount());
}

TEST(SmallPageAllocatorAllocateAll) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate all 512 pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 512; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }

    ASSERT_FALSE(allocator.allFree());
    ASSERT_TRUE(allocator.allFull());
    ASSERT_EQ(0u, allocator.freePageCount());
}

TEST(SmallPageAllocatorFreeSingle) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate and then free
    SmallPageIndex page = allocator.allocateSmallPage();
    ASSERT_EQ(511u, allocator.freePageCount());

    allocator.freeSmallPage(page);
    ASSERT_TRUE(allocator.allFree());
    ASSERT_EQ(512u, allocator.freePageCount());
}

TEST(SmallPageAllocatorFreeMultiple) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate 20 pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 20; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }
    ASSERT_EQ(492u, allocator.freePageCount());

    // Free 10 pages
    for (size_t i = 0; i < 10; i++) {
        allocator.freeSmallPage(pages[i]);
    }
    ASSERT_EQ(502u, allocator.freePageCount());
}

TEST(SmallPageAllocatorFreeAll) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate all pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 512; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }
    ASSERT_TRUE(allocator.allFull());

    // Free all pages
    for (size_t i = 0; i < 512; i++) {
        allocator.freeSmallPage(pages[i]);
    }
    ASSERT_TRUE(allocator.allFree());
    ASSERT_EQ(512u, allocator.freePageCount());
}

TEST(SmallPageAllocatorAllocateFreePattern) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate-free-allocate-free pattern
    SmallPageIndex page1 = allocator.allocateSmallPage();
    ASSERT_EQ(511u, allocator.freePageCount());

    SmallPageIndex page2 = allocator.allocateSmallPage();
    ASSERT_EQ(510u, allocator.freePageCount());

    allocator.freeSmallPage(page1);
    ASSERT_EQ(511u, allocator.freePageCount());

    SmallPageIndex page3 = allocator.allocateSmallPage();
    ASSERT_EQ(510u, allocator.freePageCount());

    allocator.freeSmallPage(page2);
    ASSERT_EQ(511u, allocator.freePageCount());

    allocator.freeSmallPage(page3);
    ASSERT_EQ(512u, allocator.freePageCount());
    ASSERT_TRUE(allocator.allFree());
}

TEST(SmallPageAllocatorReserveSinglePage) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Reserve page at index 0
    allocator.reserveSmallPage(0);

    ASSERT_FALSE(allocator.allFree());
    ASSERT_EQ(511u, allocator.freePageCount());
}

TEST(SmallPageAllocatorReserveMultiplePages) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Reserve pages at specific indices
    allocator.reserveSmallPage(0);
    allocator.reserveSmallPage(100);
    allocator.reserveSmallPage(511);

    ASSERT_FALSE(allocator.allFree());
    ASSERT_EQ(509u, allocator.freePageCount());
}

TEST(SmallPageAllocatorReserveAllPages) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Reserve all pages at once
    allocator.reserveAllPages();

    ASSERT_FALSE(allocator.allFree());
    ASSERT_TRUE(allocator.allFull());
    ASSERT_EQ(0u, allocator.freePageCount());
}

TEST(SmallPageAllocatorMixedOperations) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Mix of reserve, allocate, and free operations
    allocator.reserveSmallPage(0);
    ASSERT_EQ(511u, allocator.freePageCount());

    SmallPageIndex page1 = allocator.allocateSmallPage();
    ASSERT_EQ(510u, allocator.freePageCount());

    allocator.reserveSmallPage(100);
    ASSERT_EQ(509u, allocator.freePageCount());

    SmallPageIndex page2 = allocator.allocateSmallPage();
    ASSERT_EQ(508u, allocator.freePageCount());

    allocator.freeSmallPage(page1);
    ASSERT_EQ(509u, allocator.freePageCount());

    SmallPageIndex page3 = allocator.allocateSmallPage();
    ASSERT_EQ(508u, allocator.freePageCount());

    allocator.freeSmallPage(page2);
    allocator.freeSmallPage(page3);
    ASSERT_EQ(510u, allocator.freePageCount());
}

TEST(SmallPageAllocatorAllocatedPagesAreUnique) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate 50 pages and verify they're all unique
    std::set<SmallPageIndex> allocatedPages;
    for (size_t i = 0; i < 50; i++) {
        SmallPageIndex page = allocator.allocateSmallPage();
        ASSERT_TRUE(allocatedPages.find(page) == allocatedPages.end());
        allocatedPages.insert(page);
    }

    ASSERT_EQ(50u, allocatedPages.size());
}

TEST(SmallPageAllocatorFreeInReverseOrder) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 30; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }
    ASSERT_EQ(482u, allocator.freePageCount());

    // Free in reverse order
    for (int i = 29; i >= 0; i--) {
        allocator.freeSmallPage(pages[i]);
    }

    ASSERT_TRUE(allocator.allFree());
    ASSERT_EQ(512u, allocator.freePageCount());
}

TEST(SmallPageAllocatorFreeInRandomOrder) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate pages
    std::vector<SmallPageIndex> pages;
    for (size_t i = 0; i < 40; i++) {
        pages.push_back(allocator.allocateSmallPage());
    }
    ASSERT_EQ(472u, allocator.freePageCount());

    // Free in a mixed order: evens first, then odds
    for (size_t i = 0; i < 40; i += 2) {
        allocator.freeSmallPage(pages[i]);
    }
    ASSERT_EQ(492u, allocator.freePageCount());

    for (size_t i = 1; i < 40; i += 2) {
        allocator.freeSmallPage(pages[i]);
    }
    ASSERT_TRUE(allocator.allFree());
    ASSERT_EQ(512u, allocator.freePageCount());
}

TEST(SmallPageAllocatorReserveAfterAllocations) {
    PageAllocatorTestSetup setup;

    SmallPageAllocatorBuffers buffers;
    SmallPageAllocator allocator(buffers.fwb, buffers.bwb);

    // Allocate some pages
    SmallPageIndex page1 = allocator.allocateSmallPage();
    SmallPageIndex page2 = allocator.allocateSmallPage();
    ASSERT_EQ(510u, allocator.freePageCount());

    // Reserve a page
    allocator.reserveSmallPage(200);
    ASSERT_EQ(509u, allocator.freePageCount());

    // Allocate more
    SmallPageIndex page3 = allocator.allocateSmallPage();
    ASSERT_EQ(508u, allocator.freePageCount());

    // Free allocated pages
    allocator.freeSmallPage(page1);
    allocator.freeSmallPage(page2);
    allocator.freeSmallPage(page3);
    ASSERT_EQ(511u, allocator.freePageCount());
    ASSERT_FALSE(allocator.allFree()); // Still have reserved page
}
