//
// Unit tests for SmallPageAllocator and PageRef
// Created by Spencer Martin on 2/18/26.
//

#include "../test.h"
#include <TestHarness.h>
#include <cstdlib>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>

#include <mem/PageAllocator.h>

using namespace kernel::mm;
using namespace CroCOSTest;

constexpr phys_addr testBaseAddr(0x200000); // 2MiB aligned base

// Helper macro: set up an allocator where pages are in the ring buffer.
// allocAll exhausts lazy init, then freeing pages puts them in the ring buffer.
#define MAKE_SPA_WITH_FREE_PAGES(name, freeCount) \
    SmallPageAllocator name(testBaseAddr); \
    do { \
        name.allocAll(); \
        std::vector<PageRef> _toFree; \
        _toFree.reserve(freeCount); \
        for (size_t _i = 0; _i < (freeCount); _i++) { \
            _toFree.push_back(PageRef::small(testBaseAddr + _i * arch::smallPageSize)); \
        } \
        name.free(_toFree.data(), _toFree.size()); \
    } while(0)

// ============================================================================
// PageRef Basics
// ============================================================================

TEST(PageRef_SmallPageCreation) {
    auto ref = PageRef::small(phys_addr(0x1000));
    ASSERT_EQ(PageSize::SMALL, ref.size());
    ASSERT_EQ(0x1000ul, ref.addr().value);
}

TEST(PageRef_BigPageCreation) {
    auto ref = PageRef::big(phys_addr(0x200000));
    ASSERT_EQ(PageSize::BIG, ref.size());
    ASSERT_EQ(0x200000ul, ref.addr().value);
}

TEST(PageRef_RunLength) {
    auto ref = PageRef::small(phys_addr(0x1000));
    ASSERT_EQ(1ul, ref.runLength());

    ref.setRunLength(42);
    ASSERT_EQ(42ul, ref.runLength());
    ASSERT_EQ(0x1000ul, ref.addr().value);
    ASSERT_EQ(PageSize::SMALL, ref.size());
}

TEST(PageRef_RunLengthMaxValue) {
    auto ref = PageRef::small(phys_addr(0x1000));
    ref.setRunLength(PageAllocator::smallPagesPerBigPage);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, ref.runLength());
}

TEST(PageRef_Equality) {
    auto a = PageRef::small(phys_addr(0x1000));
    auto b = PageRef::small(phys_addr(0x1000));
    auto c = PageRef::small(phys_addr(0x2000));

    ASSERT_EQ(a, b);
    ASSERT_NE(a, c);
}

TEST(PageRef_RunLengthPreservesSize) {
    auto small = PageRef::small(phys_addr(0x1000));
    small.setRunLength(10);
    ASSERT_EQ(PageSize::SMALL, small.size());

    auto big = PageRef::big(phys_addr(0x200000));
    big.setRunLength(10);
    ASSERT_EQ(PageSize::BIG, big.size());
}

// ============================================================================
// Construction and Initial State
// ============================================================================

TEST(SmallPageAllocator_InitiallyEmpty) {
    SmallPageAllocator spa(testBaseAddr);
    ASSERT_TRUE(spa.isEmpty());
    ASSERT_FALSE(spa.isFull());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, spa.freePageCount());
}

// ============================================================================
// allocAll / freeAll
// ============================================================================

TEST(SmallPageAllocator_AllocAllMakesFull) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    ASSERT_TRUE(spa.isFull());
    ASSERT_FALSE(spa.isEmpty());
    ASSERT_EQ(0ul, spa.freePageCount());
}

TEST(SmallPageAllocator_FreeAllMakesEmpty) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    spa.freeAll();
    ASSERT_TRUE(spa.isEmpty());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, spa.freePageCount());
}

TEST(SmallPageAllocator_AllocAllThenFreeAllRoundtrip) {
    SmallPageAllocator spa(testBaseAddr);
    for (int i = 0; i < 3; i++) {
        spa.allocAll();
        ASSERT_TRUE(spa.isFull());
        spa.freeAll();
        ASSERT_TRUE(spa.isEmpty());
    }
}

// ============================================================================
// Allocation via Lazy Init Path
// ============================================================================

TEST(SmallPageAllocator_AllocSinglePage) {
    SmallPageAllocator spa(testBaseAddr);

    PageRef allocated = INVALID_PAGE_REF;
    size_t count = spa.alloc([&](PageRef ref) { allocated = ref; }, 1);

    ASSERT_EQ(1ul, count);
    ASSERT_NE(INVALID_PAGE_REF, allocated);
    ASSERT_EQ(PageSize::SMALL, allocated.size());
    ASSERT_GE(allocated.addr().value, testBaseAddr.value);
    ASSERT_LT(allocated.addr().value, testBaseAddr.value + arch::bigPageSize);
}

TEST(SmallPageAllocator_AllocAllPagesViaLazyInit) {
    SmallPageAllocator spa(testBaseAddr);

    std::set<uint64_t> addresses;
    size_t count = spa.alloc([&](PageRef ref) {
        addresses.insert(ref.addr().value);
    }, PageAllocator::smallPagesPerBigPage);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, count);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, addresses.size());
    ASSERT_TRUE(spa.isFull());
}

// ============================================================================
// Allocation from Ring Buffer (via allocAll + free setup)
// ============================================================================

TEST(SmallPageAllocator_AllocSinglePageFromRingBuffer) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    PageRef allocated = INVALID_PAGE_REF;
    size_t count = spa.alloc([&](PageRef ref) { allocated = ref; }, 1);

    ASSERT_EQ(1ul, count);
    ASSERT_NE(INVALID_PAGE_REF, allocated);
    ASSERT_EQ(PageSize::SMALL, allocated.size());
    ASSERT_GE(allocated.addr().value, testBaseAddr.value);
    ASSERT_LT(allocated.addr().value, testBaseAddr.value + arch::bigPageSize);
}

TEST(SmallPageAllocator_BulkAllocFromRingBuffer) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 64);

    std::vector<PageRef> pages;
    size_t count = spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 64);

    ASSERT_EQ(64ul, count);
    ASSERT_EQ(64ul, pages.size());

    std::set<uint64_t> addresses;
    for (auto& p : pages) {
        addresses.insert(p.addr().value);
    }
    ASSERT_EQ(64ul, addresses.size());
}

TEST(SmallPageAllocator_AllocAllPagesFromRingBuffer) {
    MAKE_SPA_WITH_FREE_PAGES(spa, PageAllocator::smallPagesPerBigPage);

    std::vector<PageRef> pages;
    size_t count = spa.alloc([&](PageRef ref) { pages.push_back(ref); },
                             PageAllocator::smallPagesPerBigPage);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, count);
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_AllocWhenFullReturnsZero) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();

    size_t count = spa.alloc([](PageRef) {}, 1);
    ASSERT_EQ(0ul, count);
}

TEST(SmallPageAllocator_AllocZeroPages) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);
    size_t count = spa.alloc([](PageRef) { ASSERT_UNREACHABLE("should not be called"); }, 0);
    ASSERT_EQ(0ul, count);
}

TEST(SmallPageAllocator_PartialAllocFromRingBuffer) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    std::vector<PageRef> pages;
    size_t count = spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 20);

    ASSERT_EQ(10ul, count);
    ASSERT_TRUE(spa.isFull());
}

// ============================================================================
// Free and Reallocate
// ============================================================================

TEST(SmallPageAllocator_FreeAndReallocate) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    // Allocate all 10
    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 10);
    ASSERT_EQ(10ul, pages.size());
    ASSERT_TRUE(spa.isFull());

    // Free them back
    spa.free(pages.data(), pages.size());
    ASSERT_EQ(10ul, spa.freePageCount());

    // Reallocate
    std::vector<PageRef> pages2;
    size_t count = spa.alloc([&](PageRef ref) { pages2.push_back(ref); }, 10);
    ASSERT_EQ(10ul, count);
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_AllocFreeCycle) {
    MAKE_SPA_WITH_FREE_PAGES(spa, PageAllocator::smallPagesPerBigPage);

    // Exhaust all pages
    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); },
              PageAllocator::smallPagesPerBigPage);
    ASSERT_TRUE(spa.isFull());

    // Free all
    spa.free(pages.data(), pages.size());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, spa.freePageCount());

    // Allocate again
    std::vector<PageRef> pages2;
    size_t count = spa.alloc([&](PageRef ref) { pages2.push_back(ref); },
                             PageAllocator::smallPagesPerBigPage);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, count);
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_MultipleAllocFreeCycles) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 32);

    for (int cycle = 0; cycle < 5; cycle++) {
        std::vector<PageRef> pages;
        size_t count = spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 32);
        ASSERT_EQ(32ul, count);
        ASSERT_TRUE(spa.isFull());

        spa.free(pages.data(), pages.size());
        ASSERT_EQ(32ul, spa.freePageCount());
    }
}

// ============================================================================
// Address Properties
// ============================================================================

TEST(SmallPageAllocator_AddressesArePageAligned) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 32);

    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 32);

    for (auto& p : pages) {
        ASSERT_EQ(0ul, p.addr().value % arch::smallPageSize);
    }
}

TEST(SmallPageAllocator_AddressesWithinBigPage) {
    MAKE_SPA_WITH_FREE_PAGES(spa, PageAllocator::smallPagesPerBigPage);

    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); },
              PageAllocator::smallPagesPerBigPage);

    for (auto& p : pages) {
        ASSERT_GE(p.addr().value, testBaseAddr.value);
        ASSERT_LT(p.addr().value, testBaseAddr.value + arch::bigPageSize);
    }
}

TEST(SmallPageAllocator_AllAllocatedPagesAreUnique) {
    MAKE_SPA_WITH_FREE_PAGES(spa, PageAllocator::smallPagesPerBigPage);

    std::set<uint64_t> addresses;
    spa.alloc([&](PageRef ref) { addresses.insert(ref.addr().value); },
              PageAllocator::smallPagesPerBigPage);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, addresses.size());
}

// ============================================================================
// freePageCount Consistency
// ============================================================================

TEST(SmallPageAllocator_FreePageCountDecreasesOnAlloc) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 20);
    size_t initial = spa.freePageCount();
    ASSERT_EQ(20ul, initial);

    spa.alloc([](PageRef) {}, 10);
    ASSERT_EQ(10ul, spa.freePageCount());
}

TEST(SmallPageAllocator_FreePageCountIncreasesOnFree) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 10);
    ASSERT_EQ(10ul, pages.size());
    ASSERT_EQ(0ul, spa.freePageCount());

    spa.free(pages.data(), 5);
    ASSERT_EQ(5ul, spa.freePageCount());

    spa.free(pages.data() + 5, 5);
    ASSERT_EQ(10ul, spa.freePageCount());
}

TEST(SmallPageAllocator_FreePageCountZeroWhenFull) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    ASSERT_EQ(0ul, spa.freePageCount());
}

// ============================================================================
// isPageFree (public API)
// ============================================================================

TEST(SmallPageAllocator_IsPageFreeAfterAllocAll) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();

    // After allocAll, pages are occupied
    PageRef testPage = PageRef::small(testBaseAddr);
    ASSERT_FALSE(spa.isPageFree(testPage));
}

TEST(SmallPageAllocator_IsPageFreeAfterFreeAll) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    spa.freeAll();

    // After freeAll, pages are free
    PageRef testPage = PageRef::small(testBaseAddr);
    ASSERT_TRUE(spa.isPageFree(testPage));
}

TEST(SmallPageAllocator_IsPageFreeAfterRingBufferAlloc) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 10);

    // After alloc from ring buffer, pages are occupied
    for (auto& p : pages) {
        ASSERT_FALSE(spa.isPageFree(p));
    }
}

TEST(SmallPageAllocator_IsPageFreeAfterLazyInitAlloc) {
    SmallPageAllocator spa(testBaseAddr);

    std::vector<PageRef> pages;
    spa.alloc([&](PageRef ref) { pages.push_back(ref); }, 5);

    for (auto& p : pages) {
        ASSERT_FALSE(spa.isPageFree(p));
    }
}

// ============================================================================
// reservePage
// ============================================================================

TEST(SmallPageAllocator_ReservePageMarksOccupied) {
    SmallPageAllocator spa(testBaseAddr);

    // Before reserve, page is free
    PageRef page = PageRef::small(testBaseAddr);
    ASSERT_TRUE(spa.isPageFree(page));

    spa.reservePage(testBaseAddr);

    // After reserve, page is occupied
    ASSERT_FALSE(spa.isPageFree(page));
}

TEST(SmallPageAllocator_ReservedPageSkippedDuringAlloc) {
    SmallPageAllocator spa(testBaseAddr);
    spa.reservePage(testBaseAddr);

    // Allocate all pages via lazy init
    std::set<uint64_t> allocatedAddrs;
    spa.alloc([&](PageRef ref) { allocatedAddrs.insert(ref.addr().value); },
              PageAllocator::smallPagesPerBigPage);

    // Reserved page should not have been allocated
    ASSERT_EQ(0ul, allocatedAddrs.count(testBaseAddr.value));
}

TEST(SmallPageAllocator_ReserveMultiplePages) {
    SmallPageAllocator spa(testBaseAddr);

    for (size_t i = 0; i < 4; i++) {
        spa.reservePage(testBaseAddr + i * arch::smallPageSize);
    }

    // All reserved pages should be marked occupied
    for (size_t i = 0; i < 4; i++) {
        PageRef page = PageRef::small(testBaseAddr + i * arch::smallPageSize);
        ASSERT_FALSE(spa.isPageFree(page));
    }

    // Non-reserved page should still be free
    PageRef unreserved = PageRef::small(testBaseAddr + 4 * arch::smallPageSize);
    ASSERT_TRUE(spa.isPageFree(unreserved));

    // Allocate and verify none of the reserved addresses appear
    std::set<uint64_t> allocatedAddrs;
    spa.alloc([&](PageRef ref) { allocatedAddrs.insert(ref.addr().value); },
              PageAllocator::smallPagesPerBigPage);

    for (size_t i = 0; i < 4; i++) {
        phys_addr reserved = testBaseAddr + i * arch::smallPageSize;
        ASSERT_EQ(0ul, allocatedAddrs.count(reserved.value));
    }
}

// ============================================================================
// Zero-Count Edge Cases
// ============================================================================

TEST(SmallPageAllocator_FreeZeroPages) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);
    size_t before = spa.freePageCount();
    spa.free(nullptr, 0);
    ASSERT_EQ(before, spa.freePageCount());
}

// ============================================================================
// Double-Free Detection
// ============================================================================

TEST(SmallPageAllocator_DoubleFreeDetected) {
    MAKE_SPA_WITH_FREE_PAGES(spa, 10);

    // Allocate a page
    PageRef page = INVALID_PAGE_REF;
    spa.alloc([&](PageRef ref) { page = ref; }, 1);
    ASSERT_NE(INVALID_PAGE_REF, page);

    // First free should succeed
    spa.free(&page, 1);

    // Second free of the same page should trigger an assertion
    bool exceptionCaught = false;
    try {
        spa.free(&page, 1);
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("already free") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

TEST(SmallPageAllocator_FreeUnallocatedPageDetected) {
    SmallPageAllocator spa(testBaseAddr);

    // Try to free a page that was never allocated (still in lazy-init range)
    PageRef page = PageRef::small(testBaseAddr);

    bool exceptionCaught = false;
    try {
        spa.free(&page, 1);
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("already free") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

// ============================================================================
// Concurrent Tests
// ============================================================================

TEST(SmallPageAllocator_ConcurrentLazyInitAlloc) {
    // Multiple threads race to allocate pages through the lazy init CAS path.
    // All allocated pages must be unique and the total must equal the big page.
    constexpr size_t numThreads = 4;
    constexpr size_t pagesPerThread = PageAllocator::smallPagesPerBigPage / numThreads;

    SmallPageAllocator spa(testBaseAddr);
    std::atomic<bool> start{false};

    std::mutex perThreadMutex[numThreads];
    std::vector<uint64_t> perThread[numThreads];

    auto worker = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        std::vector<uint64_t> local;
        spa.alloc([&](PageRef ref) {
            local.push_back(ref.addr().value);
        }, pagesPerThread);
        std::lock_guard<std::mutex> lock(perThreadMutex[id]);
        perThread[id] = std::move(local);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back(worker, i);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    // Collect all allocated addresses
    std::set<uint64_t> allAddresses;
    size_t totalAllocated = 0;
    for (size_t i = 0; i < numThreads; i++) {
        totalAllocated += perThread[i].size();
        for (auto addr : perThread[i]) {
            allAddresses.insert(addr);
        }
    }

    // Every page should be unique (no double-allocation)
    ASSERT_EQ(totalAllocated, allAddresses.size());
    // Total should cover the entire big page
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, totalAllocated);

    // All addresses should be within range and aligned
    for (auto addr : allAddresses) {
        ASSERT_GE(addr, testBaseAddr.value);
        ASSERT_LT(addr, testBaseAddr.value + arch::bigPageSize);
        ASSERT_EQ(0ul, addr % arch::smallPageSize);
    }
}

TEST(SmallPageAllocator_ConcurrentRingBufferAlloc) {
    // Multiple threads race to allocate from the ring buffer path.
    constexpr size_t numThreads = 4;
    constexpr size_t totalPages = PageAllocator::smallPagesPerBigPage;
    constexpr size_t pagesPerThread = totalPages / numThreads;

    MAKE_SPA_WITH_FREE_PAGES(spa, totalPages);
    std::atomic<bool> start{false};

    std::mutex perThreadMutex[numThreads];
    std::vector<uint64_t> perThread[numThreads];

    auto worker = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        std::vector<uint64_t> local;
        spa.alloc([&](PageRef ref) {
            local.push_back(ref.addr().value);
        }, pagesPerThread);
        std::lock_guard<std::mutex> lock(perThreadMutex[id]);
        perThread[id] = std::move(local);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back(worker, i);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::set<uint64_t> allAddresses;
    size_t totalAllocated = 0;
    for (size_t i = 0; i < numThreads; i++) {
        totalAllocated += perThread[i].size();
        for (auto addr : perThread[i]) {
            allAddresses.insert(addr);
        }
    }

    ASSERT_EQ(totalAllocated, allAddresses.size());
    ASSERT_EQ(totalPages, totalAllocated);
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_ConcurrentAllocAndFree) {
    // Producer threads free pages back into the allocator while consumer
    // threads allocate them. Tests the ring buffer under concurrent read/write.
    constexpr size_t numAllocators = 2;
    constexpr size_t numFreers = 2;
    constexpr size_t pagesPerFreer = 64;
    constexpr size_t totalPages = numFreers * pagesPerFreer;

    // Start fully allocated so lazy init is exhausted, then free pages
    // from the freer threads into the ring buffer.
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();

    std::atomic<bool> start{false};
    std::atomic<size_t> totalFreed{0};
    std::atomic<size_t> totalAllocated{0};

    // Each freer owns a disjoint set of page indices to free
    auto freer = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        size_t base = id * pagesPerFreer;
        // Free in small batches to interleave with allocators
        for (size_t i = 0; i < pagesPerFreer; i += 4) {
            size_t batch = std::min(size_t(4), pagesPerFreer - i);
            PageRef refs[4];
            for (size_t j = 0; j < batch; j++) {
                refs[j] = PageRef::small(testBaseAddr + (base + i + j) * arch::smallPageSize);
            }
            spa.free(refs, batch);
            totalFreed.fetch_add(batch, std::memory_order_relaxed);
        }
    };

    std::mutex allocMutex[numAllocators];
    std::vector<uint64_t> perAllocator[numAllocators];

    auto allocator = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        std::vector<uint64_t> local;
        size_t myTotal = 0;
        size_t target = totalPages / numAllocators;
        while (myTotal < target) {
            size_t got = spa.alloc([&](PageRef ref) {
                local.push_back(ref.addr().value);
            }, std::min(size_t(4), target - myTotal));
            myTotal += got;
        }
        std::lock_guard<std::mutex> lock(allocMutex[id]);
        perAllocator[id] = std::move(local);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numFreers; i++) {
        threads.emplace_back(freer, i);
    }
    for (size_t i = 0; i < numAllocators; i++) {
        threads.emplace_back(allocator, i);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    // Collect all allocated addresses
    std::set<uint64_t> allAddresses;
    size_t allocated = 0;
    for (size_t i = 0; i < numAllocators; i++) {
        allocated += perAllocator[i].size();
        for (auto addr : perAllocator[i]) {
            allAddresses.insert(addr);
        }
    }

    // Every allocated page should be unique
    ASSERT_EQ(allocated, allAddresses.size());
    // Should have allocated all freed pages
    ASSERT_EQ(totalPages, allocated);
}

TEST(SmallPageAllocator_ConcurrentAllocFreeCycles) {
    // Multiple threads independently do alloc-then-free cycles on the same
    // allocator, stress-testing the ring buffer and lazy init atomics.
    constexpr size_t numThreads = 4;
    constexpr size_t cyclesPerThread = 2000;
    constexpr size_t pagesPerCycle = 32;

    // Set up with enough pages in the ring buffer for all threads
    MAKE_SPA_WITH_FREE_PAGES(spa, numThreads * pagesPerCycle);
    std::atomic<bool> start{false};
    std::atomic<size_t> successfulCycles{0};

    auto worker = [&](size_t) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t cycle = 0; cycle < cyclesPerThread; cycle++) {
            PageRef pages[pagesPerCycle];
            size_t got = 0;
            spa.alloc([&](PageRef ref) {
                pages[got++] = ref;
            }, pagesPerCycle);

            if (got > 0) {
                spa.free(pages, got);
                successfulCycles.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back(worker, i);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    // At least some cycles should have succeeded
    ASSERT_GT(successfulCycles.load(), 0ul);
    // All pages should be back (each thread frees what it allocates)
    ASSERT_EQ(numThreads * pagesPerCycle, spa.freePageCount());
}
