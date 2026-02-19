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
#include <core/ds/LinkedList.h>
#include <mem/PageAllocatorTuning.h>
#include <core/Iterator.h>
#include <core/ds/Trees.h>

#include <mem/mm.h>

#define PA_BITMAP_ITERATOR_CACHE_WORD

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

struct PoolID {
    using IDType = SmallestUInt_t<sizeof(arch::ProcessorID) * 8 + 1>;
    IDType id;

    constexpr PoolID(const arch::ProcessorID pid) : id(pid) {}
    constexpr PoolID() : id(static_cast<IDType>(-1)) {}

    bool operator==(const PoolID &) const = default;
    [[nodiscard]] bool global() const;
};

constexpr PoolID GLOBAL = {};
using BigPageColor = SmallestUInt_t<log2ceil(MAX_COLOR_COUNT)>;
constexpr BigPageColor uncolored = MAX_COLOR_COUNT;

enum class BigPageState : uint8_t {
    FREE,
    FULL,
    PARTIALLY_ALLOCATED
};

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


struct BigPageMetadata {

};

struct BigPageLinkedListExtractor {
    static BigPageMetadata*& previous(BigPageMetadata& m);
    static BigPageMetadata*& next(BigPageMetadata& m);
};

struct BigPageColoredLinkedListExtractor {
    static BigPageMetadata*& previous(BigPageMetadata& m);
    static BigPageMetadata*& next(BigPageMetadata& m);
};

enum class PoolPressure : uint8_t {
    SURPLUS = 0,
    COMFORTABLE = 1,
    MODERATE = 2,
    DESPERATE = 3,

    COUNT
};

// Traits for converting between index types and bit indices
template<typename IndexType>
struct BitmapIndexTraits {
    // Returns the number of bits required for the given entry count
    static size_t requiredBits(size_t entryCount);
    // Converts an index to a bit index
    static size_t toBitIndex(IndexType index, size_t entryCount);
    // Converts a bit index back to an index
    static IndexType fromBitIndex(size_t bitIndex, size_t entryCount);
};

// Specialization for PoolID: handles global pool as last bit
// For N processors, we need N+1 bits (0..N-1 for processors, N for global)
template<>
struct BitmapIndexTraits<PoolID> {
    static size_t requiredBits(size_t processorCount) {
        return processorCount + 1;
    }
    static size_t toBitIndex(PoolID pool, size_t processorCount) {
        return pool.global() ? processorCount : pool.id;
    }
    static PoolID fromBitIndex(size_t bitIndex, size_t processorCount) {
        if (bitIndex == processorCount) {
            return GLOBAL;
        }
        return PoolID(static_cast<arch::ProcessorID>(bitIndex));
    }
};

// Specialization for size_t: direct mapping, no special handling
template<>
struct BitmapIndexTraits<size_t> {
    static size_t requiredBits(size_t rangeCount) {
        return rangeCount;
    }
    static size_t toBitIndex(size_t index, size_t) {
        return index;
    }
    static size_t fromBitIndex(size_t bitIndex, size_t) {
        return bitIndex;
    }
};

template<typename IndexType>
class PressureBitmap {
    using Traits = BitmapIndexTraits<IndexType>;

    Atomic<uint64_t>* bitmaps[static_cast<size_t>(PoolPressure::COUNT)];
    size_t entryCount;
public:
    class BitmapIterator {
        Atomic<uint64_t>* bitmapStart;
        const size_t totalBits;      // Actual number of bits in bitmap
        const size_t entryCount;     // Semantic count for traits conversion
#ifdef PA_BITMAP_ITERATOR_CACHE_WORD
        uint64_t currentWord;
#endif
        size_t index;

        void advanceToSetBit();

    public:
        BitmapIterator(Atomic<uint64_t>* bitmap, size_t index, size_t totalBits, size_t entryCount);
        [[nodiscard]] bool atEnd() const;
        BitmapIterator& operator++();
        bool operator==(const BitmapIterator& other) const;
        IndexType operator*() const;
    };

    static void measureAllocation(BootstrapAllocator& allocator, size_t entryCount);
    PressureBitmap(BootstrapAllocator& allocator, size_t entryCount);

    // Move constructor and assignment for lazy initialization
    PressureBitmap(PressureBitmap&& other) noexcept;
    PressureBitmap& operator=(PressureBitmap&& other) noexcept;

    // Delete copy operations
    PressureBitmap(const PressureBitmap&) = delete;
    PressureBitmap& operator=(const PressureBitmap&) = delete;

    void markPressure(IndexType index, PoolPressure pressure);

    [[nodiscard]] IteratorRange<BitmapIterator> poolsWithPressure(PoolPressure pressure) const;
};

enum class AllocationDesperation : uint8_t{
    RELAXED,
    MODERATE,
    DESPERATE
};

struct BigPagePool {

};

#endif //CROCOS_PAGEALLOCATOR_H