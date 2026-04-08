//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATOR_H
#define CROCOS_PAGEALLOCATOR_H

#include <mem/MemTypes.h>
#include <arch.h>
#include <core/utility.h>
#include <core/atomic.h>
#include <core/atomic/RingBuffer.h>
#include <mem/NUMA.h>

#include <mem/mm.h>

// ==================== Struct Definitions ====================

class BootstrapAllocator {
    uint8_t* current;
    uint8_t* end;
    bool measuring;

public:
    // Dry-run constructor (measuring mode)
    BootstrapAllocator();
    // The actual allocator initializer
    BootstrapAllocator(void* buffer, size_t size);

    template<typename T>
    T* allocate(size_t count = 1);
    [[nodiscard]] size_t bytesNeeded() const;
    [[nodiscard]] size_t bytesRemaining() const;
    [[nodiscard]] bool isFake() const {return measuring;}
};

struct PageRef {
    uint64_t value;

    static PageRef small(kernel::mm::phys_addr addr);
    static PageRef big(kernel::mm::phys_addr addr);

    [[nodiscard]] kernel::mm::PageSize size() const;
    [[nodiscard]] kernel::mm::phys_addr addr() const;

    void setRunLength(size_t);
    [[nodiscard]] size_t runLength() const;

    bool operator==(const PageRef& other) const {return other.value == value;}
} __attribute__((packed));

constexpr auto INVALID_PAGE_REF = PageRef{static_cast<uint64_t>(-1)};

using PageAllocationCallback = FunctionRef<void(PageRef)>;

// ==================== Small Page Allocator ====================

class SmallPageAllocator {
    using SmallPageIndex = SmallestUInt_t<log2ceil(kernel::mm::PageAllocator::smallPagesPerBigPage)>;
    using SmallPageCount = SmallestUInt_t<log2ceil(kernel::mm::PageAllocator::smallPagesPerBigPage + 1)>;
    constexpr static size_t bitmapWordCount = kernel::mm::PageAllocator::smallPagesPerBigPage / (8 * sizeof(uint64_t));

    SmallPageIndex buffer[kernel::mm::PageAllocator::smallPagesPerBigPage];
    Atomic<uint64_t> occupiedBitmap[bitmapWordCount]{};
    MPMCRingBuffer<SmallPageIndex, false> ringBuffer;
    kernel::mm::phys_addr baseAddr;

    Atomic<SmallPageCount> lazilyInitialized = 0;
    SmallPageCount reservedCount = 0;

    [[nodiscard]] bool isPageFree(SmallPageIndex index) const;
    bool markPageFreeState(SmallPageIndex index, bool isFree);
    [[nodiscard]] kernel::mm::phys_addr fromPageIndex(SmallPageIndex index) const;

public:
    explicit SmallPageAllocator(kernel::mm::phys_addr base);

    [[nodiscard]] bool isPageFree(PageRef page) const;
    void free(PageRef* pages, size_t count);
    size_t alloc(PageAllocationCallback cb, size_t count);
    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] size_t freePageCount() const;
    void freeAll();
    void allocAll();
    void reservePage(kernel::mm::phys_addr addr);
};

// ==================== Big Page Metadata ====================

class NUMAPool;

class BigPageMetadata {
    SmallPageAllocator& subpageAllocator;
    NUMAPool& ownerPool;

public:
    BigPageMetadata(SmallPageAllocator& allocator, NUMAPool& pool);

    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb);
    void freePages(PageRef* pages, size_t count);

    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;

    [[nodiscard]] NUMAPool& getOwnerPool() const { return ownerPool; }
};

// ==================== Multi-Level Bit Pool ====================

class BitPool {
    struct alignas(64) CacheLineEntry {
        Atomic<uint64_t> hintBitmap;
        Atomic<int64_t> count;
        uint8_t padding[48];  // 64 - 8 - 8 = 48 bytes padding
    };

    struct Level {
        CacheLineEntry* entries;
        size_t numWords;
    };

    struct L0Location {
        size_t l1EntryIndex;  // Which L1 entry (which L0 word)
        size_t bitIndex;      // Which bit in that word
    };

    Level* levels;
    size_t numLevels;
    Atomic<uint64_t>* l0Bitmap;  // L0 is just bitmap

    [[nodiscard]] L0Location pageToL0Location(size_t pageIndex) const {
        // In inverted tree:
        // - Bottom (numLevels - 1) levels use 6 bits each
        // - Top bits index within L0 word (up to 64 bits)

        size_t bitsForTree = (numLevels - 1) * 6;
        size_t l1EntryIndex = pageIndex & ((1ULL << bitsForTree) - 1);
        size_t l0BitIndex = pageIndex >> bitsForTree;

        return {l1EntryIndex, l0BitIndex};
    }

    bool tryClaimBit(size_t pageIndex);
    bool tryClearBit(size_t pageIndex);
    void incrementCounts(size_t pageIndex);
    void decrementCounts(size_t pageIndex);

    // Recursive get with rotation-based selection
    bool tryGetFromSubtree(size_t level, size_t wordIndex, size_t cpuId,
                          size_t retryCount, size_t& outPageIndex);

public:
    BitPool(size_t numPages, void* storage);

    // Add a PA page to the pool (returns false if already in pool)
    bool add(size_t bitIndex);

    // Remove a specific PA page from the pool (returns false if not in pool)
    bool remove(size_t bitIndex);

    // Get any PA page from the pool (returns false if pool is empty or contended)
    bool getAny(size_t cpuId, size_t& outPageIndex, size_t maxRetries = 8);

    static size_t levelsRequired(size_t numPages);
    static size_t storageRequired(size_t numPages);
};

#endif //CROCOS_PAGEALLOCATOR_H