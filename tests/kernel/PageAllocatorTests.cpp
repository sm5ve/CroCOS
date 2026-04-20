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
#include <random>
#include <chrono>

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

TEST(PageRef_Equality) {
    auto a = PageRef::small(phys_addr(0x1000));
    auto b = PageRef::small(phys_addr(0x1000));
    auto c = PageRef::small(phys_addr(0x2000));

    ASSERT_EQ(a, b);
    ASSERT_NE(a, c);
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
        ASSERT_TRUE(message.find("Double free") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

TEST(SmallPageAllocator_FreeUnallocatedPageDetected) {
    // The split-bitmap design detects a double-free via freeBitmap: if the bit
    // is already set when free() is called, the assert fires.  Free a page
    // twice without an intervening alloc to exercise this path.
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();

    PageRef page = PageRef::small(testBaseAddr);
    spa.free(&page, 1);   // first free — sets bit in freeBitmap

    bool exceptionCaught = false;
    try {
        spa.free(&page, 1);  // second free — bit already set → assert
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("Double free") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

// ============================================================================
// Concurrent Tests
// ============================================================================

TEST(SmallPageAllocator_DrainAllocBitmap) {
    // The split-bitmap design uses a single alloc CPU.  Verify that a single
    // thread can drain all 512 pages from a fresh allocator, producing unique,
    // in-range, aligned addresses.
    SmallPageAllocator spa(testBaseAddr);

    std::set<uint64_t> allAddresses;
    spa.alloc([&](PageRef ref) {
        allAddresses.insert(ref.addr().value);
    }, PageAllocator::smallPagesPerBigPage);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, allAddresses.size());
    for (auto addr : allAddresses) {
        ASSERT_GE(addr, testBaseAddr.value);
        ASSERT_LT(addr, testBaseAddr.value + arch::bigPageSize);
        ASSERT_EQ(0ul, addr % arch::smallPageSize);
    }
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_DrainFreeBitmap) {
    // Verify the freeBitmap→allocBitmap refresh path: start fully allocated,
    // free all pages into freeBitmap, then a single alloc thread drains them.
    constexpr size_t totalPages = PageAllocator::smallPagesPerBigPage;

    MAKE_SPA_WITH_FREE_PAGES(spa, totalPages);

    std::set<uint64_t> allAddresses;
    spa.alloc([&](PageRef ref) {
        allAddresses.insert(ref.addr().value);
    }, totalPages);

    ASSERT_EQ(totalPages, allAddresses.size());
    ASSERT_TRUE(spa.isFull());
}

TEST(SmallPageAllocator_ConcurrentAllocAndFree) {
    // One alloc thread + two free threads: freers concurrently write to
    // freeBitmap while the single alloc thread refreshes from it.
    // Verifies no double-allocation and that all freed pages are recovered.
    constexpr size_t numFreers = 2;
    constexpr size_t pagesPerFreer = 64;
    constexpr size_t totalPages = numFreers * pagesPerFreer;

    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();

    std::atomic<bool> start{false};
    std::atomic<size_t> totalFreed{0};

    // Each freer owns a disjoint set of page addresses.
    auto freer = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        size_t base = id * pagesPerFreer;
        for (size_t i = 0; i < pagesPerFreer; i += 4) {
            size_t batch = std::min(size_t(4), pagesPerFreer - i);
            PageRef refs[4];
            for (size_t j = 0; j < batch; j++)
                refs[j] = PageRef::small(testBaseAddr + (base + i + j) * arch::smallPageSize);
            spa.free(refs, batch);
            totalFreed.fetch_add(batch, std::memory_order_relaxed);
        }
    };

    // Single alloc thread: spin-allocates until it has recovered all freed pages.
    std::vector<uint64_t> allocAddresses;
    auto allocator = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (allocAddresses.size() < totalPages) {
            spa.alloc([&](PageRef ref) {
                allocAddresses.push_back(ref.addr().value);
            }, totalPages - allocAddresses.size());
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < numFreers; i++) threads.emplace_back(freer, i);
    threads.emplace_back(allocator);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    ASSERT_EQ(totalPages, allocAddresses.size());
    std::set<uint64_t> unique(allocAddresses.begin(), allocAddresses.end());
    ASSERT_EQ(totalPages, unique.size());
}

// ============================================================================
// Test Scaffolding for NUMAPool and PageAllocatorImpl
// ============================================================================
//
// The two-pass construction pattern (measure → allocate buffer → construct)
// is handled automatically. Physical address layout for tests:
//
//   testDomainBase(d) — 1GiB-aligned base for domain slot d, never overlapping.
//   makeBigPageRange(base, n) — n big pages starting at base.
//
// Typical usage:
//
//   auto pool = TestNUMAPool::withBigPages(testDomainBase(0), 8);
//   pool.pool->allocatePages(...);
//
//   TestPageAllocatorImpl impl({
//       DomainSpec::simple(0, 8, {0, 1}),
//       DomainSpec::simple(1, 4, {2, 3}),
//   });
//   // impl.impl       — the live PageAllocatorImpl
//   // impl.localPools — per-CPU LocalPool pointers (indexed by ProcessorID)

static constexpr uint64_t testDomainBase(size_t domain) {
    // Start at 1GiB to avoid colliding with the low-memory/BIOS region.
    return (domain + 1) * (1ull << 30);
}

static phys_memory_range makeBigPageRange(uint64_t base, size_t bigPageCount) {
    return { phys_addr(base), phys_addr(base + bigPageCount * arch::bigPageSize) };
}

// BootstrapBuffer — RAII backing storage for a BootstrapAllocator.
// Zeroed on construction so uninitialised atomic members start from a
// well-defined state even when placement-new isn't run on them.
struct BootstrapBuffer {
    std::vector<uint8_t> storage;

    explicit BootstrapBuffer(size_t bytes) : storage(bytes, 0) {}

    BootstrapAllocator makeAllocator() {
        return BootstrapAllocator(storage.data(), storage.size());
    }
};

// TestNUMAPool — RAII owner of a NUMAPool and its backing memory.
//
// Runs both passes automatically:
//   1. Measuring pass  → determines how much backing memory is needed.
//   2. Real pass       → constructs the pool inside the allocated buffer.
//
// The buffer outlives the pool pointer; both are destroyed together.
// Non-copyable (buffer contents are address-sensitive).  All factory
// methods below are eligible for C++17 mandatory copy elision.
struct TestNUMAPool {
    Vector<phys_memory_range> ranges;
    BootstrapBuffer           buffer;
    NUMAPool*                 pool = nullptr;

    explicit TestNUMAPool(Vector<phys_memory_range> r)
        : ranges(move(r))
        , buffer(measure(ranges))
    {
        BootstrapAllocator real = buffer.makeAllocator();
        pool = createNumaPool(real, ranges);
    }

    // Single contiguous range of bigPageCount big pages.
    static TestNUMAPool withBigPages(uint64_t base, size_t bigPageCount) {
        Vector<phys_memory_range> r;
        r.push(makeBigPageRange(base, bigPageCount));
        return TestNUMAPool(move(r));
    }

    // rangeCount discontiguous ranges, each bigPagesPerRange big pages wide,
    // laid out consecutively from base.  Models a pool whose memory comes
    // from several separate physical spans.
    static TestNUMAPool withRanges(uint64_t base, size_t rangeCount,
                                   size_t bigPagesPerRange) {
        Vector<phys_memory_range> r;
        for (size_t i = 0; i < rangeCount; i++) {
            r.push(makeBigPageRange(
                base + i * bigPagesPerRange * arch::bigPageSize, bigPagesPerRange));
        }
        return TestNUMAPool(move(r));
    }

private:
    static size_t measure(const Vector<phys_memory_range>& r) {
        BootstrapAllocator measuring;
        createNumaPool(measuring, r);
        return measuring.bytesNeeded();
    }
};

// DomainSpec — configuration for one NUMA domain in TestPageAllocatorImpl.
struct DomainSpec {
    Vector<phys_memory_range> ranges;
    std::vector<size_t>       cpuIds; // logical CPU IDs owned by this domain

    // Convenience: a single range at testDomainBase(domainSlot) covering
    // bigPageCount big pages, assigned to cpuIds.
    static DomainSpec simple(size_t domainSlot, size_t bigPageCount,
                             std::vector<size_t> cpuIds) {
        Vector<phys_memory_range> r;
        r.push(makeBigPageRange(testDomainBase(domainSlot), bigPageCount));
        return { move(r), move(cpuIds) };
    }
};

// TestPageAllocatorImpl — RAII owner of a PageAllocatorImpl and all its
// backing memory (one BootstrapBuffer per NUMA domain).
//
// Mirrors the structure of initPageAllocator() exactly so that any
// bugs caught here would also manifest in the real initialisation path.
//
// IMPORTANT: domainBuffers is reserved before the construction loop so that
// push_back() never reallocates.  If it did, the raw pointers held inside
// the BootstrapAllocators in perDomainAllocs would be invalidated.
//
// NOTE: The domains vector must be provided in contiguous domain-ID order
// (domains[0] = domain 0, domains[1] = domain 1, ...) to satisfy the
// createPageAllocator contract that numaPools[domainID] is valid.
// The test helpers below always use contiguous IDs starting at 0.
struct TestPageAllocatorImpl {
    std::vector<BootstrapBuffer>              domainBuffers;
    LocalPool*                                localPools[arch::MAX_PROCESSOR_COUNT] = {};
    kernel::numa::NUMATopology                topology = kernel::numa::NUMATopology::build(
        kernel::numa::EmptyIterable<kernel::numa::ProcessorAffinityEntry>{},
        kernel::numa::EmptyIterable<kernel::numa::MemoryRangeAffinityEntry>{},
        kernel::numa::EmptyIterable<kernel::numa::GenericInitiatorEntry>{}
    ); // replaced in constructor when domains are provided
    std::optional<kernel::numa::NUMAPolicy>   policy;
    PageAllocatorImpl                         impl;

    explicit TestPageAllocatorImpl(std::vector<DomainSpec> domains) {
        domainBuffers.reserve(domains.size()); // prevent reallocation; see note above
        Vector<NUMAPool*> numaPools;

        // Determine total processor count (highest CPU ID + 1) across all domains.
        size_t processorCount = 0;
        for (auto& spec : domains) {
            for (size_t cpu : spec.cpuIds) {
                if (cpu + 1 > processorCount) processorCount = cpu + 1;
            }
        }

        // Build a NUMATopology with the CPU-to-domain assignments from the specs.
        std::vector<kernel::numa::ProcessorAffinityEntry> procEntries;
        for (size_t di = 0; di < domains.size(); di++) {
            for (size_t cpu : domains[di].cpuIds) {
                procEntries.push_back({
                    static_cast<arch::ProcessorID>(cpu),
                    static_cast<uint32_t>(di),
                    static_cast<uint32_t>(di)  // clock domain same as proximity domain
                });
            }
        }
        if (!procEntries.empty()) {
            topology = kernel::numa::NUMATopology::build(
                procEntries,
                kernel::numa::EmptyIterable<kernel::numa::MemoryRangeAffinityEntry>{},
                kernel::numa::EmptyIterable<kernel::numa::GenericInitiatorEntry>{}
            );
        }

        // For multi-domain topologies, build a NUMAPolicy so createPageAllocator
        // can populate cpuNearestPool correctly.
        const kernel::numa::NUMAPolicy* policyPtr = nullptr;
        if (domains.size() > 1) {
            policy.emplace(topology);
            policyPtr = &policy.value();
        }

        for (size_t di = 0; di < domains.size(); di++) {
            auto& spec = domains[di];
            kernel::numa::DomainID domainId{static_cast<uint16_t>(di)};

            // ---- Measuring pass: determine memory requirements for this domain ----
            BootstrapAllocator measuring;
            createNumaPool(measuring, spec.ranges, domainId);
            for (size_t i = 0; i < spec.cpuIds.size(); i++) {
                createLocalPool(measuring, &topology);
            }

            // ---- Allocate buffer (reserve guarantees no reallocation here) ----
            domainBuffers.emplace_back(measuring.bytesNeeded());

            // ---- Real pass: construct NUMAPool and LocalPools in the buffer ----
            BootstrapAllocator real = domainBuffers.back().makeAllocator();
            NUMAPool* domainPool = createNumaPool(real, spec.ranges, domainId);
            numaPools.push(domainPool);
            for (size_t cpu : spec.cpuIds) {
                localPools[cpu] = createLocalPool(real, &topology, domainPool);
            }
        }

        impl = createPageAllocator(move(numaPools), localPools, processorCount,
                                   nullptr, policyPtr);
    }
};

// ============================================================================
// Scaffold Validation
// ============================================================================
// These tests verify that the two-pass construction scaffolding above works
// correctly.  Detailed behavioural tests for NUMAPool and PageAllocatorImpl
// will be added here as createNumaPool and createPageAllocator are implemented.

TEST(NUMAPool_Scaffold_SingleRange) {
    auto pool = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    ASSERT_NE(nullptr, pool.pool);
}

TEST(NUMAPool_Scaffold_MultipleRanges) {
    // Pool whose physical memory comes from 3 separate spans of 4 big pages each
    auto pool = TestNUMAPool::withRanges(testDomainBase(0), 3, 4);
    ASSERT_NE(nullptr, pool.pool);
}

TEST(NUMAPool_Scaffold_LargePool) {
    auto pool = TestNUMAPool::withBigPages(testDomainBase(0), 64);
    ASSERT_NE(nullptr, pool.pool);
}

TEST(PageAllocatorImpl_Scaffold_SingleDomain) {
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 8, {0, 1, 2, 3}),
    });
    ASSERT_NE(nullptr, impl.localPools[0]);
    ASSERT_NE(nullptr, impl.localPools[1]);
    ASSERT_NE(nullptr, impl.localPools[2]);
    ASSERT_NE(nullptr, impl.localPools[3]);
}

TEST(PageAllocatorImpl_Scaffold_TwoDomains) {
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 8, {0, 1}),
        DomainSpec::simple(1, 8, {2, 3}),
    });
    ASSERT_NE(nullptr, impl.localPools[0]);
    ASSERT_NE(nullptr, impl.localPools[2]);
    // Each domain allocates its LocalPools from a separate buffer, so pointers
    // belonging to different domains must be distinct.
    ASSERT_NE(impl.localPools[0], impl.localPools[2]);
}

TEST(PageAllocatorImpl_Scaffold_AsymmetricDomains) {
    // Validates that per-domain buffer sizing is computed independently —
    // a large domain shouldn't force a small domain to over-allocate (or
    // vice versa cause an underflow).
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 1,  {0}),
        DomainSpec::simple(1, 64, {1, 2, 3, 4, 5, 6, 7}),
    });
    ASSERT_NE(nullptr, impl.localPools[0]);
    ASSERT_NE(nullptr, impl.localPools[1]);
}

// ============================================================================
// SmallPageAllocator — Occupancy state machine
// ============================================================================

TEST(SPA_Transition_EmptyToPartial) {
    SmallPageAllocator spa(testBaseAddr);
    OccupancyTransition t{};
    spa.alloc([](PageRef){}, 1, t);

    ASSERT_EQ(OccupancyState::Empty,   t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(t.becameAvailable());
    ASSERT_FALSE(spa.isEmpty());
    ASSERT_FALSE(spa.isFull());
}

TEST(SPA_Transition_EmptyToFull) {
    SmallPageAllocator spa(testBaseAddr);
    OccupancyTransition t{};
    spa.alloc([](PageRef){}, PageAllocator::smallPagesPerBigPage, t);

    ASSERT_EQ(OccupancyState::Empty, t.before);
    ASSERT_EQ(OccupancyState::Full,  t.after);
    ASSERT_TRUE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(t.becameAvailable());
    ASSERT_FALSE(spa.isEmpty());
    ASSERT_TRUE(spa.isFull());
}

TEST(SPA_Transition_PartialToFull) {
    SmallPageAllocator spa(testBaseAddr);
    spa.alloc([](PageRef){}, 1);
    OccupancyTransition t{};
    spa.alloc([](PageRef){}, PageAllocator::smallPagesPerBigPage - 1, t);

    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Full,    t.after);
    ASSERT_TRUE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(t.becameAvailable());
    ASSERT_TRUE(spa.isFull());
}

TEST(SPA_Transition_FullToPartial) {
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    PageRef p = PageRef::small(testBaseAddr);
    OccupancyTransition t{};
    spa.free(&p, 1, t);

    ASSERT_EQ(OccupancyState::Full,    t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_TRUE(t.becameAvailable());
    ASSERT_FALSE(spa.isEmpty());
    ASSERT_FALSE(spa.isFull());
}

TEST(SPA_Transition_PartialToEmpty) {
    SmallPageAllocator spa(testBaseAddr);
    std::vector<PageRef> pages;
    spa.alloc([&](PageRef r){ pages.push_back(r); }, 10);
    OccupancyTransition t{};
    spa.free(pages.data(), pages.size(), t);

    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Empty,   t.after);
    ASSERT_TRUE(t.becameEmpty());
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameAvailable());
    ASSERT_TRUE(spa.isEmpty());
    ASSERT_FALSE(spa.isFull());
}

TEST(SPA_Transition_FullToEmpty) {
    // Alloc all, then free all in one call.
    // Both becameEmpty() and becameAvailable() must be true — this is the exact
    // case handled by the if/else-if ordering in freeSmallPageRun.
    SmallPageAllocator spa(testBaseAddr);
    spa.allocAll();
    std::vector<PageRef> pages;
    pages.reserve(PageAllocator::smallPagesPerBigPage);
    for (size_t i = 0; i < PageAllocator::smallPagesPerBigPage; i++) {
        pages.push_back(PageRef::small(testBaseAddr + i * arch::smallPageSize));
    }
    OccupancyTransition t{};
    spa.free(pages.data(), pages.size(), t);

    ASSERT_EQ(OccupancyState::Full,  t.before);
    ASSERT_EQ(OccupancyState::Empty, t.after);
    ASSERT_TRUE(t.becameEmpty());
    ASSERT_TRUE(t.becameAvailable());  // Full → non-Full
    ASSERT_FALSE(t.becameFull());
    ASSERT_TRUE(spa.isEmpty());
}

TEST(SPA_Transition_PartialStaysPartial) {
    SmallPageAllocator spa(testBaseAddr);
    // Alloc 100 pages (discarded), then alloc 50 more (saved to free back).
    spa.alloc([](PageRef){}, 100);
    std::vector<PageRef> toFree;
    spa.alloc([&](PageRef r){ toFree.push_back(r); }, 50);
    OccupancyTransition t{};
    spa.free(toFree.data(), toFree.size(), t);

    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(t.becameAvailable());
}

// ============================================================================
// SmallPageAllocator — Address correctness
// ============================================================================

TEST(SPA_Addresses_AllUnique) {
    SmallPageAllocator spa(testBaseAddr);
    std::set<uint64_t> seen;
    spa.alloc([&](PageRef r){ seen.insert(r.addr().value); }, PageAllocator::smallPagesPerBigPage);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, seen.size());
}

TEST(SPA_Addresses_InRange) {
    SmallPageAllocator spa(testBaseAddr);
    bool allInRange = true;
    spa.alloc([&](PageRef r){
        if (r.addr().value < testBaseAddr.value ||
            r.addr().value >= testBaseAddr.value + arch::bigPageSize) {
            allInRange = false;
        }
    }, PageAllocator::smallPagesPerBigPage);
    ASSERT_TRUE(allInRange);
}

TEST(SPA_Addresses_Aligned) {
    SmallPageAllocator spa(testBaseAddr);
    bool allAligned = true;
    spa.alloc([&](PageRef r){
        if (r.addr().value % arch::smallPageSize != 0) allAligned = false;
    }, PageAllocator::smallPagesPerBigPage);
    ASSERT_TRUE(allAligned);
}

// ============================================================================
// SmallPageAllocator — Lazy init and ring buffer
// ============================================================================

TEST(SPA_LazyInit_FirstPageIsBase) {
    SmallPageAllocator spa(testBaseAddr);
    PageRef got = INVALID_PAGE_REF;
    spa.alloc([&](PageRef r){ got = r; }, 1);
    ASSERT_EQ(testBaseAddr.value, got.addr().value);
}

TEST(SPA_LazyInit_Sequential) {
    constexpr size_t N = 8;
    SmallPageAllocator spa(testBaseAddr);
    std::vector<PageRef> pages;
    spa.alloc([&](PageRef r){ pages.push_back(r); }, N);
    ASSERT_EQ(N, pages.size());
    for (size_t i = 0; i < N; i++) {
        ASSERT_EQ(testBaseAddr.value + i * arch::smallPageSize, pages[i].addr().value);
    }
}

TEST(SPA_RingBuffer_ReturnsFreedPages) {
    SmallPageAllocator spa(testBaseAddr);
    // Exhaust lazy init by allocating everything, then free 5 specific pages.
    spa.alloc([](PageRef){}, PageAllocator::smallPagesPerBigPage - 5);
    std::vector<PageRef> freed;
    spa.alloc([&](PageRef r){ freed.push_back(r); }, 5);
    spa.free(freed.data(), freed.size());

    // Reallocate 5 pages — they must come from the ring buffer.
    std::set<uint64_t> expected;
    for (const auto& pg : freed) expected.insert(pg.addr().value);

    std::set<uint64_t> reallocated;
    spa.alloc([&](PageRef r){ reallocated.insert(r.addr().value); }, 5);
    ASSERT_EQ(expected, reallocated);
}

// ============================================================================
// SmallPageAllocator — Reserved pages
// ============================================================================

TEST(SPA_Reserved_NeverAllocated) {
    SmallPageAllocator spa(testBaseAddr);
    spa.reservePage(testBaseAddr);

    std::set<uint64_t> allocated;
    spa.alloc([&](PageRef r){ allocated.insert(r.addr().value); },
              PageAllocator::smallPagesPerBigPage - 1);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 1, allocated.size());
    ASSERT_EQ(0u, allocated.count(testBaseAddr.value));
}

TEST(SPA_Reserved_IsFullAt511) {
    SmallPageAllocator spa(testBaseAddr);
    spa.reservePage(testBaseAddr);
    constexpr size_t maxAlloc = PageAllocator::smallPagesPerBigPage - 1;

    spa.alloc([](PageRef){}, maxAlloc - 1);
    ASSERT_FALSE(spa.isFull());

    spa.alloc([](PageRef){}, 1);
    ASSERT_TRUE(spa.isFull());
}

TEST(SPA_Reserved_FreePageCount) {
    SmallPageAllocator spa(testBaseAddr);
    spa.reservePage(testBaseAddr);
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 1, spa.freePageCount());
}

// ============================================================================
// SmallPageAllocator — White-box invariant checks
// ============================================================================

TEST(SPA_Invariants_FreshAllocator) {
    // Fresh allocator: all 512 pages are free in allocBitmap, none allocated.
    SmallPageAllocator spa(testBaseAddr);
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(0u, spa.getAllocatedCount());
    ASSERT_EQ(0u, spa.getReservedCount());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, spa.getBitmapPopcount());
}

TEST(SPA_Invariants_AfterPartialAlloc) {
    // After allocating 100 pages, 412 remain free in allocBitmap.
    SmallPageAllocator spa(testBaseAddr);
    spa.alloc([](PageRef){}, 100);
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(100u, spa.getAllocatedCount());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 100, spa.getBitmapPopcount());
}

TEST(SPA_Invariants_AfterAllocFree) {
    SmallPageAllocator spa(testBaseAddr);
    // Alloc all, free 200, alloc 100 from ring buffer.
    spa.allocAll();
    std::vector<PageRef> freed;
    freed.reserve(200);
    for (size_t i = 0; i < 200; i++) {
        freed.push_back(PageRef::small(testBaseAddr + i * arch::smallPageSize));
    }
    spa.free(freed.data(), freed.size());
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 200, spa.getAllocatedCount());

    spa.alloc([](PageRef){}, 100);
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 100, spa.getAllocatedCount());
}

TEST(SPA_Invariants_WithReserved) {
    // Reserved pages are removed from allocBitmap and excluded from both bitmaps.
    SmallPageAllocator spa(testBaseAddr);
    spa.reservePage(testBaseAddr);
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(1u, spa.getReservedCount());
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 1, spa.getBitmapPopcount());
    ASSERT_EQ(0u, spa.getAllocatedCount());

    spa.alloc([](PageRef){}, 50);
    ASSERT_TRUE(spa.checkInvariants());
    // 512 - 1 reserved - 50 allocated = 461 free pages remain in bitmaps.
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage - 51, spa.getBitmapPopcount());
    ASSERT_EQ(50u, spa.getAllocatedCount());
}

// ============================================================================
// NUMAPool — Initialization
// ============================================================================

TEST(NUMAPool_Init_AllPagesInFreeBigPages) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    ASSERT_EQ(4u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Init_BigPageOnlyAllocSucceeds) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    size_t count = 0;
    BigPageMetadata* rem = nullptr;
    size_t got = p.pool->allocatePages(4 * PageAllocator::smallPagesPerBigPage,
                                       [&](PageRef){ count++; }, rem,
                                       AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(4 * PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(4u, count);
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
}

// ============================================================================
// NUMAPool — BIG_PAGE_ONLY allocation
// ============================================================================

TEST(NUMAPool_BigPageOnly_ReturnsCorrectCount) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    size_t cbCount = 0;
    BigPageMetadata* rem = nullptr;
    size_t got = p.pool->allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                                       [&](PageRef){ cbCount++; }, rem,
                                       AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(2 * PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(2u, cbCount);
    ASSERT_EQ(2u, p.pool->getFreeBigPageCount());
}

TEST(NUMAPool_BigPageOnly_PageRefSizeIsBig) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ pages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    for (const auto& pg : pages) {
        ASSERT_EQ(PageSize::BIG, pg.size());
    }
}

TEST(NUMAPool_BigPageOnly_ExhaustPool) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                          [](PageRef){}, rem, AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());

    size_t extra = 0;
    size_t got = p.pool->allocatePages(PageAllocator::smallPagesPerBigPage,
                                       [&](PageRef){ extra++; }, rem,
                                       AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(0u, got);
    ASSERT_EQ(0u, extra);
}

// ============================================================================
// NUMAPool — Small-page allocation
// ============================================================================

TEST(NUMAPool_SmallPage_AllocFew) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    size_t got = p.pool->allocatePages(10, [&](PageRef r){ pages.push_back(r); }, rem);
    ASSERT_EQ(10u, got);
    ASSERT_EQ(10u, pages.size());
    ASSERT_NE(nullptr, rem);
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
}

TEST(NUMAPool_SmallPage_PageRefSizeIsSmall) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(10, [&](PageRef r){ pages.push_back(r); }, rem);
    for (const auto& pg : pages) {
        ASSERT_EQ(PageSize::SMALL, pg.size());
    }
}

TEST(NUMAPool_SmallPage_FillOneBigPage) {
    // Fill a big page using two alloc calls to force the small-page path.
    // A single call of exactly 512 would be served via the whole-big-page path
    // and return a big PageRef instead of small ones.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    constexpr size_t total = PageAllocator::smallPagesPerBigPage;

    // First alloc: partial page lands in rem (not yet in paPages).
    p.pool->allocatePages(total - 1, [&](PageRef r){ pages.push_back(r); }, rem);
    ASSERT_EQ(total - 1, pages.size());
    ASSERT_NE(nullptr, rem);
    rem->returnPage();  // register partial page in paPages

    // Second alloc: exhausts the page from paPages.
    rem = nullptr;
    p.pool->allocatePages(1, [&](PageRef r){ pages.push_back(r); }, rem);
    ASSERT_EQ(total, pages.size());
    ASSERT_EQ(nullptr, rem);  // page became full; no partial page to return
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
    for (const auto& pg : pages) {
        ASSERT_EQ(PageSize::SMALL, pg.size());
    }
}

TEST(NUMAPool_SmallPage_PARemainingPartial) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(100, [&](PageRef r){ pages.push_back(r); }, rem);
    // rem holds the partially-used big page; it is partial (not empty, not full).
    ASSERT_NE(nullptr, rem);
    ASSERT_FALSE(rem->isEmpty());
    ASSERT_FALSE(rem->isFull());
}

// ============================================================================
// NUMAPool — Free routing (occupancy transitions)
// ============================================================================

// Helper: fill a 1-big-page pool with small-page allocs and return all pages.
// Uses two calls to force the small-page path (single call of 512 hits big-page path).
static std::vector<PageRef> fillPoolSmallPages(TestNUMAPool& p) {
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    constexpr size_t total = PageAllocator::smallPagesPerBigPage;
    p.pool->allocatePages(total - 1, [&](PageRef r){ pages.push_back(r); }, rem);
    rem->returnPage();
    rem = nullptr;
    p.pool->allocatePages(1, [&](PageRef r){ pages.push_back(r); }, rem);
    return pages;
}

TEST(NUMAPool_Routing_FullToPartial_GoesToPAPages) {
    // Full → Partial: freed pages should land in paPages, not freeBigPages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    auto pages = fillPoolSmallPages(p);
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());

    p.pool->freePages(pages.data(), 1);
    ASSERT_EQ(1u, p.pool->getPAPagesCount());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Routing_FullToEmpty_GoesToFreeBigPages) {
    // Full → Empty: freeing all pages at once should return the big page to freeBigPages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    auto pages = fillPoolSmallPages(p);

    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Routing_PartialToEmpty_GoesToFreeBigPages) {
    // Partial → Empty: freeing all allocated pages should return the big page to freeBigPages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(100, [&](PageRef r){ pages.push_back(r); }, rem);
    rem->returnPage();  // register partial page in paPages
    ASSERT_EQ(1u, p.pool->getPAPagesCount());

    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Routing_FreeBigPage_GoesToFreeBigPages) {
    // Freeing a big PageRef should return it to freeBigPages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    ASSERT_EQ(2u, p.pool->getFreeBigPageCount());

    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ pages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(1u, pages.size());
    ASSERT_EQ(PageSize::BIG, pages[0].size());

    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(2u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

// ============================================================================
// NUMAPool — Run detection in freePages()
// ============================================================================

TEST(NUMAPool_RunDetection_MultipleBigPages) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(3 * PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ pages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(3u, pages.size());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());

    // Free all 3 big pages in a single freePages() call.
    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(4u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_RunDetection_MultipleSmallSameSuperpage) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(100, [&](PageRef r){ pages.push_back(r); }, rem);
    rem->returnPage();  // register in paPages so Partial→Empty routing works
    ASSERT_EQ(1u, p.pool->getPAPagesCount());

    // Free all 100 small pages in one call: Partial→Empty → freeBigPages.
    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_RunDetection_MixedBigAndSmall) {
    // One big-page free + one small-page run in the same freePages() call.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);

    // Alloc 1 whole big page.
    std::vector<PageRef> bigPages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ bigPages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(1u, bigPages.size());

    // Alloc 10 small pages from the remaining big page; register rem in paPages.
    std::vector<PageRef> smallPages;
    rem = nullptr;
    p.pool->allocatePages(10, [&](PageRef r){ smallPages.push_back(r); }, rem);
    ASSERT_NE(nullptr, rem);
    rem->returnPage();  // necessary for Partial→Empty routing in freeSmallPageRun
    ASSERT_EQ(1u, p.pool->getPAPagesCount());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());

    // Combine and free everything in one call.
    std::vector<PageRef> allPages;
    allPages.insert(allPages.end(), bigPages.begin(), bigPages.end());
    allPages.insert(allPages.end(), smallPages.begin(), smallPages.end());
    p.pool->freePages(allPages.data(), allPages.size());

    ASSERT_EQ(2u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

// ============================================================================
// NUMAPool — Round-trips and invariants
// ============================================================================

TEST(NUMAPool_RoundTrip_AllocBigFreeAll) {
    constexpr size_t N = 4;
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), N);
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;

    p.pool->allocatePages(N * PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ pages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(N, pages.size());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());

    p.pool->freePages(pages.data(), pages.size());
    ASSERT_EQ(N, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Invariants_AfterOperations) {
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);

    // Alloc 1 big page.
    std::vector<PageRef> bigPages;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(PageAllocator::smallPagesPerBigPage,
                          [&](PageRef r){ bigPages.push_back(r); }, rem,
                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_TRUE(p.pool->checkInvariants());

    // Alloc 50 small pages from a second big page; register rem.
    std::vector<PageRef> smallPages;
    rem = nullptr;
    p.pool->allocatePages(50, [&](PageRef r){ smallPages.push_back(r); }, rem);
    ASSERT_NE(nullptr, rem);
    rem->returnPage();
    ASSERT_TRUE(p.pool->checkInvariants());

    // Free the small pages: Partial→Empty route.
    p.pool->freePages(smallPages.data(), smallPages.size());
    ASSERT_TRUE(p.pool->checkInvariants());

    // Free the big page.
    p.pool->freePages(bigPages.data(), bigPages.size());
    ASSERT_TRUE(p.pool->checkInvariants());

    ASSERT_EQ(4u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
}

// ============================================================================
// BigPageMetadata — Direct interface tests
// ============================================================================
// All tests below obtain a BigPageMetadata* via findMetadata() and operate
// on it directly, bypassing NUMAPool routing.  This isolates the BigPageMetadata
// state machine from the pool-level tracking.

TEST(BigPageMetadata_BaseAddr) {
    // baseAddr() should return the big-page-aligned base passed at construction.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);
    ASSERT_EQ(testDomainBase(0), meta->baseAddr().value);
}

TEST(BigPageMetadata_GetOwnerPool) {
    // getOwnerPool() should return the NUMAPool that constructed this metadata.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);
    ASSERT_EQ(p.pool, &meta->getOwnerPool());
}

TEST(BigPageMetadata_HasReservedSubpages_FreshPage) {
    // A freshly created big page has no reserved subpages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);
    ASSERT_FALSE(meta->hasReservedSubpages());
}

TEST(BigPageMetadata_OccupancyTransition_EmptyToPartial) {
    // Allocating fewer than the full capacity from an empty page yields
    // an Empty→Partial transition.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);
    ASSERT_TRUE(meta->isEmpty());

    OccupancyTransition t{};
    meta->allocatePages(10, [](PageRef){}, t);

    ASSERT_EQ(OccupancyState::Empty,   t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(meta->isEmpty());
    ASSERT_FALSE(meta->isFull());
}

TEST(BigPageMetadata_OccupancyTransition_EmptyToFull) {
    // Allocating all pages from an empty page yields an Empty→Full transition.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);

    OccupancyTransition t{};
    meta->allocatePages(PageAllocator::smallPagesPerBigPage, [](PageRef){}, t);

    ASSERT_EQ(OccupancyState::Empty, t.before);
    ASSERT_EQ(OccupancyState::Full,  t.after);
    ASSERT_TRUE(t.becameFull());
    ASSERT_TRUE(meta->isFull());
    ASSERT_FALSE(meta->isEmpty());
}

TEST(BigPageMetadata_OccupancyTransition_FullToPartial) {
    // Freeing one page from a full page yields a Full→Partial transition.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);

    std::vector<PageRef> pages;
    meta->allocatePages(PageAllocator::smallPagesPerBigPage,
                        [&](PageRef r){ pages.push_back(r); });
    ASSERT_TRUE(meta->isFull());

    OccupancyTransition t{};
    meta->freePages(pages.data(), 1, t);

    ASSERT_EQ(OccupancyState::Full,    t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_TRUE(t.becameAvailable());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(meta->isFull());
    ASSERT_FALSE(meta->isEmpty());
}

TEST(BigPageMetadata_OccupancyTransition_PartialToEmpty) {
    // Freeing all allocated pages from a partial page yields a Partial→Empty transition.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);

    std::vector<PageRef> pages;
    meta->allocatePages(50, [&](PageRef r){ pages.push_back(r); });

    OccupancyTransition t{};
    meta->freePages(pages.data(), pages.size(), t);

    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Empty,   t.after);
    ASSERT_TRUE(t.becameEmpty());
    ASSERT_FALSE(t.becameFull());
    ASSERT_TRUE(meta->isEmpty());
}

// ============================================================================
// NUMAPool — findMetadata boundary conditions
// ============================================================================

TEST(NUMAPool_FindMetadata_AtRangeStart) {
    // The exact range-start address maps to the first big page.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(testDomainBase(0)));
    ASSERT_NE(nullptr, meta);
    ASSERT_EQ(testDomainBase(0), meta->baseAddr().value);
}

TEST(NUMAPool_FindMetadata_AtLastBigPage) {
    // The last big page's base address (rangeEnd - bigPageSize) should return
    // valid metadata for that final page.
    constexpr size_t N = 4;
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), N);
    const uint64_t lastBase = testDomainBase(0) + (N - 1) * arch::bigPageSize;
    BigPageMetadata* meta = p.pool->findMetadata(phys_addr(lastBase));
    ASSERT_NE(nullptr, meta);
    ASSERT_EQ(lastBase, meta->baseAddr().value);
}

TEST(NUMAPool_FindMetadata_BeforeRange_ReturnsNull) {
    // An address one big page before the range start is outside the pool.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 4);
    const uint64_t before = testDomainBase(0) - arch::bigPageSize;
    ASSERT_EQ(nullptr, p.pool->findMetadata(phys_addr(before)));
}

TEST(NUMAPool_FindMetadata_AtRangeEnd_ReturnsNull) {
    // rangeEnd itself is just past the last page and should not map to anything.
    constexpr size_t N = 4;
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), N);
    const uint64_t rangeEnd = testDomainBase(0) + N * arch::bigPageSize;
    ASSERT_EQ(nullptr, p.pool->findMetadata(phys_addr(rangeEnd)));
}

// ============================================================================
// NUMAPool — BIG_PAGE_ONLY overallocation
// ============================================================================

TEST(NUMAPool_BigPageOnly_Overallocates_NonDivisibleCount) {
    // BIG_PAGE_ONLY rounds the requested count UP to a whole big page.
    // Requesting just 1 small page should still return a full big page's worth.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 2);
    BigPageMetadata* rem = nullptr;
    size_t cbCount = 0;
    size_t got = p.pool->allocatePages(1, [&](PageRef r){
        ASSERT_EQ(PageSize::BIG, r.size());
        cbCount++;
    }, rem, AllocBehavior::BIG_PAGE_ONLY);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(1u, cbCount);
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());  // 1 of 2 consumed
}

// ============================================================================
// NUMAPool — Allocation fallback path
// ============================================================================
//
// The allocatePages() fallback path (lines 881-913 of NewPageAllocator.cpp):
//   1. allocateFromPAPages(RELAXED_RETRIES) — exhausts paPages or gives up quickly.
//   2. Fallback loop — grabs free big pages when paPages cannot satisfy the request.
//   3. allocateFromPAPages(DETERMINED_RETRIES) — final sweep of any newly-added paPages.
//
// The tests below exercise two distinct scenarios that reach the fallback loop.

TEST(NUMAPool_Fallback_PAPagesEmpty_AllocatesFromFreeBigPages) {
    // Scenario: paPages is empty from the start, so allocateFromPAPages(16)
    // exits immediately.  The remainder after the initial whole-big-page grab
    // must be satisfied by the fallback loop grabbing another big page and
    // performing a small-page alloc from it.
    //
    // Pool: 3 big pages, all in freeBigPages, paPages = 0.
    // Request 600 small pages:
    //   - Initial grab: floor(600/512) = 1 big page → emits BIG PageRef, smallPageCount = 88.
    //   - allocateFromPAPages(16): paPages empty → returns immediately.
    //   - Fallback loop: requiredPages = 1 → grabs 1 more, allocates 88 small from it.
    //   - Total: 512 + 88 = 600, paPageRemaining points to the partial fallback page.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 3);
    ASSERT_EQ(3u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());

    constexpr size_t request = 600;
    constexpr size_t remainder = request - PageAllocator::smallPagesPerBigPage; // 88
    std::vector<PageRef> pages;
    BigPageMetadata* rem = nullptr;
    size_t got = p.pool->allocatePages(request, [&](PageRef r){ pages.push_back(r); }, rem);

    ASSERT_EQ(request, got);
    // 1 whole big page consumed by initial grab + 1 partial big page consumed by fallback.
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    // The fallback page is partial; paPageRemaining must be non-null.
    ASSERT_NE(nullptr, rem);
    ASSERT_FALSE(rem->isEmpty());
    ASSERT_FALSE(rem->isFull());
    // Verify the mix: 1 BIG PageRef (initial grab) + remainder small PageRefs (fallback).
    size_t bigCount = 0, smallCount = 0;
    for (const auto& pg : pages) {
        if (pg.size() == PageSize::BIG) bigCount++;
        else smallCount++;
    }
    ASSERT_EQ(1u, bigCount);
    ASSERT_EQ(remainder, smallCount);
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Fallback_PAPagesExhaustedMidRequest) {
    // Scenario: paPages is non-empty but holds fewer pages than needed after
    // the initial big-page grab.  allocateFromPAPages(16) exhausts paPages
    // without satisfying the remainder, so the fallback loop handles the rest.
    //
    // Setup: alloc 462 (=512-50) small pages → paPageRemaining has 50 free.
    //        returnPage() → paPages = {bigPage0 with 50 free}, freeBigPages = 2.
    // Request 600:
    //   - Initial grab: 1 big page → smallPageCount = 88.
    //   - allocateFromPAPages(16): bigPage0 has 50 free → allocates 50, becomes full.
    //     smallPageCount = 38.  paPages now empty.
    //   - Fallback loop: requiredPages = 1 → grabs last free big page, allocs 38 small.
    //   - Total: 512 (big) + 50 (paPages) + 38 (fallback) = 600.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 3);

    constexpr size_t preAlloc = PageAllocator::smallPagesPerBigPage - 50;
    BigPageMetadata* rem = nullptr;
    p.pool->allocatePages(preAlloc, [](PageRef){}, rem);
    ASSERT_NE(nullptr, rem);
    rem->returnPage();
    ASSERT_EQ(1u, p.pool->getPAPagesCount());
    ASSERT_EQ(2u, p.pool->getFreeBigPageCount());

    constexpr size_t request = 600;
    std::vector<PageRef> pages;
    rem = nullptr;
    size_t got = p.pool->allocatePages(request, [&](PageRef r){ pages.push_back(r); }, rem);

    ASSERT_EQ(request, got);
    // bigPage0 became full (removed from paPages), both other big pages consumed.
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
    ASSERT_NE(nullptr, rem);  // fallback big page is partially used
    ASSERT_TRUE(p.pool->checkInvariants());
}

TEST(NUMAPool_Fallback_OOM_PartialSatisfaction) {
    // When the pool cannot satisfy the full request, allocatePages returns
    // however many pages were available rather than asserting or panicking.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 1);
    BigPageMetadata* rem = nullptr;
    size_t got = p.pool->allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                                       [](PageRef){}, rem);
    // Only 1 big page's worth available.
    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(0u, p.pool->getFreeBigPageCount());
    ASSERT_EQ(0u, p.pool->getPAPagesCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

// ============================================================================
// NUMAPool — Small-page runs spanning multiple big pages
// ============================================================================

TEST(NUMAPool_Free_SmallPagesFromMultipleBigPages) {
    // freePages() must dispatch each big page's small-page run to a separate
    // freeSmallPageRun() call.  This test allocates 100 small pages from two
    // different big pages and frees 50 from each in a single freePages() call,
    // verifying that both big pages remain partial and in paPages.
    auto p = TestNUMAPool::withBigPages(testDomainBase(0), 3);

    BigPageMetadata* rem0 = nullptr;
    BigPageMetadata* rem1 = nullptr;
    std::vector<PageRef> pages0, pages1;

    // First alloc: paPages is empty → fallback grabs the first free big page.
    p.pool->allocatePages(100, [&](PageRef r){ pages0.push_back(r); }, rem0);
    ASSERT_NE(nullptr, rem0);

    // Second alloc: paPages is still empty (rem0 not yet registered) → fallback
    // grabs the second free big page.  rem1 must differ from rem0.
    p.pool->allocatePages(100, [&](PageRef r){ pages1.push_back(r); }, rem1);
    ASSERT_NE(nullptr, rem1);
    ASSERT_NE(rem0, rem1);

    // Register both partial pages so freeSmallPageRun's Partial→Partial path can fire.
    rem0->returnPage();
    rem1->returnPage();
    ASSERT_EQ(2u, p.pool->getPAPagesCount());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());

    // Free 50 from each big page in one call.  After address-sort, all pages0
    // entries precede all pages1 entries, so freePages sees two distinct runs.
    std::vector<PageRef> toFree;
    toFree.insert(toFree.end(), pages0.begin(), pages0.begin() + 50);
    toFree.insert(toFree.end(), pages1.begin(), pages1.begin() + 50);
    p.pool->freePages(toFree.data(), toFree.size());

    // Each big page went Partial→Partial (50 still allocated), so both remain
    // in paPages and the free-big-page count is unchanged.
    ASSERT_EQ(2u, p.pool->getPAPagesCount());
    ASSERT_EQ(1u, p.pool->getFreeBigPageCount());
    ASSERT_TRUE(p.pool->checkInvariants());
}

// ============================================================================
// Concurrent Tests (SmallPageAllocator)
// ============================================================================

TEST(SmallPageAllocator_ConcurrentAllocFreeCycles) {
    // The split-bitmap design has a single-alloc-CPU invariant: only one thread
    // allocates at a time (non-atomic allocBitmap), while any thread may free
    // concurrently (atomic freeBitmap fetch_or).  This test validates:
    //   - One alloc thread drains allocBitmap, then refreshes from freeBitmap.
    //   - Multiple free threads write to freeBitmap concurrently with no data races.
    constexpr size_t numFreeThreads = 3;
    constexpr size_t totalCycles    = 3000;
    constexpr size_t pagesPerCycle  = 16;
    constexpr size_t totalPages     = pagesPerCycle * totalCycles;

    MAKE_SPA_WITH_FREE_PAGES(spa, pagesPerCycle);
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};

    // A lock-free queue of pages waiting to be freed by the free threads.
    // Each slot holds one page to free; producer (alloc thread) writes,
    // consumers (free threads) atomically claim slots.
    struct alignas(64) Slot {
        std::atomic<uint64_t> val{static_cast<uint64_t>(-1)};
    };
    constexpr size_t queueSize = pagesPerCycle * 4;
    std::vector<Slot> queue(queueSize);
    std::atomic<size_t> writeIdx{0};
    std::atomic<size_t> freeCount{0};
    std::atomic<size_t> allocCount{0};

    // Alloc thread: one cycle = alloc up to pagesPerCycle, enqueue each page.
    auto allocWorker = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        size_t cycles = 0;
        while (cycles < totalCycles) {
            PageRef pages[pagesPerCycle];
            size_t got = 0;
            spa.alloc([&](PageRef ref) { pages[got++] = ref; }, pagesPerCycle);
            if (got == 0) continue;
            allocCount.fetch_add(got, std::memory_order_relaxed);
            for (size_t i = 0; i < got; i++) {
                size_t slot = writeIdx.fetch_add(1, std::memory_order_relaxed) % queueSize;
                // Spin until the free threads have consumed the previous occupant.
                uint64_t expected = static_cast<uint64_t>(-1);
                while (!queue[slot].val.compare_exchange_weak(
                           expected, pages[i].value,
                           std::memory_order_release, std::memory_order_relaxed)) {
                    expected = static_cast<uint64_t>(-1);
                }
            }
            cycles++;
        }
        done.store(true, std::memory_order_release);
    };

    // Free threads: each spins scanning the queue for filled slots and frees them.
    auto freeWorker = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        size_t pos = 0;
        while (!done.load(std::memory_order_relaxed) ||
               freeCount.load(std::memory_order_relaxed) < allocCount.load(std::memory_order_relaxed)) {
            Slot& slot = queue[pos % queueSize];
            uint64_t v = slot.val.load(std::memory_order_acquire);
            if (v == static_cast<uint64_t>(-1)) { pos++; continue; }
            if (!slot.val.compare_exchange_strong(v, static_cast<uint64_t>(-1),
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                pos++; continue;
            }
            PageRef ref{v};
            spa.free(&ref, 1);
            freeCount.fetch_add(1, std::memory_order_relaxed);
            pos++;
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    threads.emplace_back(allocWorker);
    for (size_t i = 0; i < numFreeThreads; i++) threads.emplace_back(freeWorker);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    ASSERT_EQ(allocCount.load(), freeCount.load());
    ASSERT_TRUE(spa.checkInvariants());
    ASSERT_EQ(pagesPerCycle, spa.freePageCount());
}

// ============================================================================
// Concurrent Tests (NUMAPool)
// ============================================================================

TEST_WITH_TIMEOUT_NO_TRACKING(NUMAPool_ConcurrentStress_SmallPages, 8000) {
    constexpr size_t numBigPages  = 32;
    constexpr int    numThreads   = 8;
    constexpr int    numEpochs    = 50;
    constexpr int    epochMs      = 10;
    constexpr size_t maxSmallReq  = 128;

    auto p = TestNUMAPool::withBigPages(testDomainBase(0), numBigPages);

    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(t) << 32));
                std::uniform_int_distribution<size_t> pick(1, maxSmallReq);

                while (!stop.load(std::memory_order_relaxed)) {
                    PageRef pages[maxSmallReq];
                    size_t pageCount = 0;
                    BigPageMetadata* rem = nullptr;
                    size_t count = pick(rng);

                    p.pool->allocatePages(count, [&](PageRef r){ pages[pageCount++] = r; }, rem);

                    if (rem && !rem->isEmpty() && !rem->isFull())
                        rem->returnPage();

                    if (pageCount > 0)
                        p.pool->freePages(pages, pageCount);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        ASSERT_TRUE(p.pool->checkInvariants());
        ASSERT_LE(p.pool->getFreeBigPageCount() + p.pool->getPAPagesCount(), numBigPages);
    }
}

TEST_WITH_TIMEOUT_NO_TRACKING(NUMAPool_ConcurrentStress_BigPages, 8000) {
    constexpr size_t numBigPages = 32;
    constexpr int    numThreads  = 8;
    constexpr int    numEpochs   = 50;
    constexpr int    epochMs     = 10;

    auto p = TestNUMAPool::withBigPages(testDomainBase(0), numBigPages);

    pauseTracking();
    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    PageRef page[1];
                    size_t pageCount = 0;
                    BigPageMetadata* rem = nullptr;

                    p.pool->allocatePages(
                        PageAllocator::smallPagesPerBigPage,
                        [&](PageRef r){ page[pageCount++] = r; },
                        rem,
                        AllocBehavior::BIG_PAGE_ONLY);

                    if (pageCount > 0)
                        p.pool->freePages(page, pageCount);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        ASSERT_TRUE(p.pool->checkInvariants());
        ASSERT_EQ(numBigPages, p.pool->getFreeBigPageCount());
    }
}

TEST_WITH_TIMEOUT_NO_TRACKING(NUMAPool_ConcurrentStress_Mixed, 8000) {
    constexpr size_t numBigPages = 32;
    constexpr int    numThreads  = 8;
    constexpr int    numEpochs   = 50;
    constexpr int    epochMs     = 10;
    constexpr size_t maxSmallReq = 128;

    auto p = TestNUMAPool::withBigPages(testDomainBase(0), numBigPages);

    pauseTracking();
    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(t) << 32));
                std::uniform_int_distribution<size_t> smallPick(1, maxSmallReq);

                while (!stop.load(std::memory_order_relaxed)) {
                    PageRef pages[maxSmallReq];
                    size_t pageCount = 0;
                    BigPageMetadata* rem = nullptr;

                    if (rng() & 1) {
                        p.pool->allocatePages(
                            PageAllocator::smallPagesPerBigPage,
                            [&](PageRef r){ pages[pageCount++] = r; },
                            rem,
                            AllocBehavior::BIG_PAGE_ONLY);
                    } else {
                        size_t count = smallPick(rng);
                        p.pool->allocatePages(count, [&](PageRef r){ pages[pageCount++] = r; }, rem);

                        if (rem && !rem->isEmpty() && !rem->isFull())
                            rem->returnPage();
                    }

                    if (pageCount > 0)
                        p.pool->freePages(pages, pageCount);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        ASSERT_TRUE(p.pool->checkInvariants());
    }
}

TEST_WITH_TIMEOUT_NO_TRACKING(NUMAPool_ConcurrentStress_MultiRange, 8000) {
    constexpr size_t rangeCount      = 3;
    constexpr size_t bigPagesPerRange = 8;
    constexpr size_t numBigPages     = rangeCount * bigPagesPerRange;
    constexpr int    numThreads      = 8;
    constexpr int    numEpochs       = 50;
    constexpr int    epochMs         = 10;
    constexpr size_t maxSmallReq     = 128;

    auto p = TestNUMAPool::withRanges(testDomainBase(0), rangeCount, bigPagesPerRange);

    pauseTracking();
    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(t) << 32));
                std::uniform_int_distribution<size_t> pick(1, maxSmallReq);

                while (!stop.load(std::memory_order_relaxed)) {
                    PageRef pages[maxSmallReq];
                    size_t pageCount = 0;
                    BigPageMetadata* rem = nullptr;
                    size_t count = pick(rng);

                    p.pool->allocatePages(count, [&](PageRef r){ pages[pageCount++] = r; }, rem);

                    if (rem && !rem->isEmpty() && !rem->isFull())
                        rem->returnPage();

                    if (pageCount > 0)
                        p.pool->freePages(pages, pageCount);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        ASSERT_TRUE(p.pool->checkInvariants());
        ASSERT_LE(p.pool->getFreeBigPageCount() + p.pool->getPAPagesCount(), numBigPages);
    }
}

// ============================================================================
// PageAllocatorImpl — Basic Allocation (Single Domain)
// ============================================================================

TEST(PAI_SingleDomain_SmallAlloc_Basic) {
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 4, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    size_t got = impl.impl.allocatePages(10, [&](PageRef r){ pages.push_back(r); });

    ASSERT_EQ(10u, got);
    ASSERT_EQ(10u, pages.size());
}

TEST(PAI_SingleDomain_SmallAlloc_PageRefProperties) {
    // Allocated small pages must be small, page-aligned, within the domain's address range,
    // and unique (no double-allocation).
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 4, {0, 1, 2, 3}) });

    const uint64_t rangeStart = testDomainBase(0);
    const uint64_t rangeEnd   = testDomainBase(0) + 4 * arch::bigPageSize;
    std::set<uint64_t> seen;
    bool allSmall   = true;
    bool allAligned = true;
    bool allInRange = true;

    impl.impl.allocatePages(200, [&](PageRef r){
        seen.insert(r.addr().value);
        if (r.size() != PageSize::SMALL)                               allSmall   = false;
        if (r.addr().value % arch::smallPageSize != 0)                 allAligned = false;
        if (r.addr().value < rangeStart || r.addr().value >= rangeEnd) allInRange = false;
    });

    ASSERT_EQ(200u, seen.size());
    ASSERT_TRUE(allSmall);
    ASSERT_TRUE(allAligned);
    ASSERT_TRUE(allInRange);
}

TEST(PAI_SingleDomain_BigPageOnly_Basic) {
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 4, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    size_t got = impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                                          [&](PageRef r){ pages.push_back(r); },
                                          AllocBehavior::BIG_PAGE_ONLY);

    ASSERT_EQ(2 * PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(2u, pages.size());
    for (const auto& pg : pages) {
        ASSERT_EQ(PageSize::BIG, pg.size());
    }
    ASSERT_EQ(2u, impl.impl.numaPools[0]->getFreeBigPageCount());
}

TEST(PAI_SingleDomain_BigPageOnly_NonDivisibleCount) {
    // BIG_PAGE_ONLY rounds the requested count up to a whole big page.
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 2, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    size_t got = impl.impl.allocatePages(1,
                                          [&](PageRef r){ pages.push_back(r); },
                                          AllocBehavior::BIG_PAGE_ONLY);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(1u, pages.size());
    ASSERT_EQ(PageSize::BIG, pages[0].size());
}

// ============================================================================
// PageAllocatorImpl — Free (Single Domain)
// ============================================================================

TEST(PAI_SingleDomain_FreeBig_RestoredToPool) {
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 4, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                            [&](PageRef r){ pages.push_back(r); },
                            AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(2u, impl.impl.numaPools[0]->getFreeBigPageCount());

    impl.impl.freePages(pages.data(), pages.size());

    ASSERT_EQ(4u, impl.impl.numaPools[0]->getFreeBigPageCount());
    ASSERT_TRUE(impl.impl.numaPools[0]->checkInvariants());
}

TEST(PAI_SingleDomain_FreeSmall_InvariantsHold) {
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 4, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    impl.impl.allocatePages(50, [&](PageRef r){ pages.push_back(r); });
    ASSERT_EQ(50u, pages.size());

    impl.impl.freePages(pages.data(), pages.size());

    ASSERT_TRUE(impl.impl.numaPools[0]->checkInvariants());
}

TEST(PAI_SingleDomain_RoundTrip_AllocFreeAlloc) {
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 2, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                            [&](PageRef r){ pages.push_back(r); },
                            AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(2u, pages.size());
    ASSERT_EQ(0u, impl.impl.numaPools[0]->getFreeBigPageCount());

    impl.impl.freePages(pages.data(), pages.size());
    ASSERT_EQ(2u, impl.impl.numaPools[0]->getFreeBigPageCount());

    std::vector<PageRef> pages2;
    size_t got = impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                                          [&](PageRef r){ pages2.push_back(r); },
                                          AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(2 * PageAllocator::smallPagesPerBigPage, got);
    ASSERT_EQ(2u, pages2.size());
}

// ============================================================================
// PageAllocatorImpl — OOM handling
// ============================================================================

TEST(PAI_GracefulOOM_ReturnsPartialCount) {
    // GRACEFUL_OOM must not panic; it returns however many pages were available.
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 1, {0, 1, 2, 3}) });

    std::vector<PageRef> pages;
    size_t got = impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                                          [&](PageRef r){ pages.push_back(r); },
                                          AllocBehavior::GRACEFUL_OOM);

    ASSERT_EQ(PageAllocator::smallPagesPerBigPage, got);
    ASSERT_TRUE(impl.impl.numaPools[0]->checkInvariants());
}

TEST(PAI_NonGracefulOOM_Panics) {
    // Without GRACEFUL_OOM, an unsatisfiable request must trigger an assertion failure.
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 1, {0, 1, 2, 3}) });

    // Exhaust the pool so the next alloc cannot be satisfied.
    impl.impl.allocatePages(PageAllocator::smallPagesPerBigPage,
                            [](PageRef){}, AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(0u, impl.impl.numaPools[0]->getFreeBigPageCount());

    bool panicked = false;
    try {
        impl.impl.allocatePages(1, [](PageRef){});
    } catch (const AssertionFailure&) {
        panicked = true;
    }
    ASSERT_TRUE(panicked);
}

// ============================================================================
// PageAllocatorImpl — LocalPool integration
// ============================================================================

TEST(PAI_LocalPool_SecondAllocUsesCache) {
    // After a small-page alloc, the leftover portion of the consumed big page is
    // cached in the calling CPU's LocalPool.  A second alloc on the same CPU must
    // draw from that cache without touching the NUMA pool's free-big-page count.
    TestPageAllocatorImpl impl({ DomainSpec::simple(0, 2, {0, 1, 2, 3}) });

    // First alloc: NUMAPool fallback consumes 1 big page; the ~502 remaining
    // subpages are handed to the CPU's LocalPool.
    impl.impl.allocatePages(10, [](PageRef){});
    ASSERT_EQ(1u, impl.impl.numaPools[0]->getFreeBigPageCount());

    // Second alloc: must be served entirely from LocalPool — NUMAPool count unchanged.
    impl.impl.allocatePages(10, [](PageRef){});
    ASSERT_EQ(1u, impl.impl.numaPools[0]->getFreeBigPageCount());
}

// ============================================================================
// PageAllocatorImpl — Multi-domain
// ============================================================================

TEST(PAI_MultiDomain_FallbackToRemote) {
    // When the home domain is exhausted, allocatePages must spill into the remote domain.
    //
    // Domain 0: 2 big pages, CPU 0 (main thread home domain).
    // Domain 1: 2 big pages, CPU 1.
    // Request 3 big pages worth — the last one must come from domain 1.
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 2, {0}),
        DomainSpec::simple(1, 2, {1}),
    });

    constexpr size_t request = 3 * PageAllocator::smallPagesPerBigPage;
    std::vector<PageRef> pages;
    size_t got = impl.impl.allocatePages(request,
                                          [&](PageRef r){ pages.push_back(r); },
                                          AllocBehavior::GRACEFUL_OOM);

    ASSERT_EQ(request, got);
    ASSERT_EQ(0u, impl.impl.numaPools[0]->getFreeBigPageCount());
    ASSERT_EQ(1u, impl.impl.numaPools[1]->getFreeBigPageCount());
}

TEST(PAI_MultiDomain_LocalDomainOnly_NoSpill) {
    // LOCAL_DOMAIN_ONLY must never draw from remote domains, even when the local
    // domain cannot satisfy the full request.
    //
    // Domain 0: 1 big page, CPU 0.  Domain 1: 2 big pages, CPU 1.
    // Requesting 2 big pages worth with LOCAL_DOMAIN_ONLY should yield at most
    // 1 big page's worth, leaving domain 1 untouched.
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 1, {0}),
        DomainSpec::simple(1, 2, {1}),
    });

    constexpr size_t request = 2 * PageAllocator::smallPagesPerBigPage;
    size_t got = impl.impl.allocatePages(request, [](PageRef){},
                                          AllocBehavior::LOCAL_DOMAIN_ONLY |
                                          AllocBehavior::GRACEFUL_OOM);

    ASSERT_LT(got, request);
    ASSERT_EQ(2u, impl.impl.numaPools[1]->getFreeBigPageCount());
}

TEST(PAI_MultiDomain_FreeRoutesToCorrectPool) {
    // freePages() must dispatch each page back to the NUMAPool whose address range owns it.
    TestPageAllocatorImpl impl({
        DomainSpec::simple(0, 2, {0}),
        DomainSpec::simple(1, 2, {1}),
    });

    // Allocate 2 big pages from domain 0 (main thread is CPU 0, nearest pool = domain 0).
    std::vector<PageRef> pages;
    impl.impl.allocatePages(2 * PageAllocator::smallPagesPerBigPage,
                            [&](PageRef r){ pages.push_back(r); },
                            AllocBehavior::BIG_PAGE_ONLY);
    ASSERT_EQ(2u, pages.size());
    ASSERT_EQ(0u, impl.impl.numaPools[0]->getFreeBigPageCount());
    ASSERT_EQ(2u, impl.impl.numaPools[1]->getFreeBigPageCount());

    // Free them — must return to domain 0, leaving domain 1 untouched.
    impl.impl.freePages(pages.data(), pages.size());
    ASSERT_EQ(2u, impl.impl.numaPools[0]->getFreeBigPageCount());
    ASSERT_EQ(2u, impl.impl.numaPools[1]->getFreeBigPageCount());
    ASSERT_TRUE(impl.impl.numaPools[0]->checkInvariants());
    ASSERT_TRUE(impl.impl.numaPools[1]->checkInvariants());
}

// ============================================================================
// PageAllocatorImpl — Concurrent stress (single domain)
// ============================================================================

TEST_WITH_TIMEOUT_NO_TRACKING(PAI_ConcurrentStress_SingleDomain, 8000) {
    constexpr size_t numBigPages = 16;
    constexpr size_t totalPages  = numBigPages * PageAllocator::smallPagesPerBigPage;
    constexpr int    numThreads  = 4;
    constexpr int    numEpochs   = 20;
    constexpr int    epochMs     = 10;
    constexpr size_t maxPages    = 64;

    TestPageAllocatorImpl impl({ DomainSpec::simple(0, numBigPages, {0, 1, 2, 3}) });

    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        // Pages held by each thread at epoch end (final alloc, not freed).
        // Written by thread t after its loop exits; read by main after join().
        std::vector<std::vector<PageRef>> heldPages(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 rng(std::random_device{}());
                std::uniform_int_distribution<size_t> pick(1, maxPages);

                while (!stop.load(std::memory_order_relaxed)) {
                    PageRef pages[maxPages];
                    size_t pageCount = 0;
                    impl.impl.allocatePages(pick(rng),
                        [&](PageRef r){ pages[pageCount++] = r; },
                        AllocBehavior::GRACEFUL_OOM);

                    if (pageCount > 0) {
                        impl.impl.freePages(pages, pageCount);
                    }
                }

                // Final alloc: record but do NOT free — held for epoch-end inspection.
                impl.impl.allocatePages(maxPages,
                    [&](PageRef r){ heldPages[t].push_back(r); },
                    AllocBehavior::GRACEFUL_OOM);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        // ---- Epoch-end correctness checks ----

        // (1) Collect all held pages, expanding big PageRefs to small-page granularity.
        //     Every address must be unique — duplicates indicate double-allocation.
        std::set<uint64_t> allHeldSmallAddrs;
        size_t totalHeld = 0;
        for (int t = 0; t < numThreads; t++) {
            for (const auto& pg : heldPages[t]) {
                if (pg.size() == PageSize::BIG) {
                    for (size_t i = 0; i < PageAllocator::smallPagesPerBigPage; i++) {
                        bool inserted = allHeldSmallAddrs.insert(
                            pg.addr().value + i * arch::smallPageSize).second;
                        ASSERT_TRUE(inserted); // duplicate = double-allocation across threads
                    }
                    totalHeld += PageAllocator::smallPagesPerBigPage;
                } else {
                    bool inserted = allHeldSmallAddrs.insert(pg.addr().value).second;
                    ASSERT_TRUE(inserted);
                    totalHeld += 1;
                }
            }
        }

        // (2) Each held page must be marked as allocated inside the impl.
        for (int t = 0; t < numThreads; t++) {
            for (const auto& pg : heldPages[t]) {
                ASSERT_TRUE(impl.impl.isPageAllocated(pg));
            }
        }

        // (3) Free pages + held pages must equal the total pages we initialized with.
        //     countFreePages() sums freeSubpageCount() across every BigPageMetadata,
        //     covering pages in freeBigPages, paPages, and LocalPool caches.
        const size_t freeCount = impl.impl.countFreePages();
        ASSERT_EQ(totalPages, freeCount + totalHeld);

        // (4) NUMAPool structural invariant.
        ASSERT_TRUE(impl.impl.numaPools[0]->checkInvariants());
        ASSERT_LE(impl.impl.numaPools[0]->getFreeBigPageCount() +
                  impl.impl.numaPools[0]->getPAPagesCount(), numBigPages);

        // Release all held pages before the next epoch.
        for (int t = 0; t < numThreads; t++) {
            if (!heldPages[t].empty()) {
                impl.impl.freePages(heldPages[t].data(), heldPages[t].size());
            }
        }

        // After freeing everything, every page must be free.
        ASSERT_EQ(totalPages, impl.impl.countFreePages());
    }
}
