//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATOR_H
#define CROCOS_PAGEALLOCATOR_H

#include <mem/MemTypes.h>
#include <arch.h>
#include <core/utility.h>
#include <core/atomic.h>
#include <core/ds/Permutation.h>
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

struct SmallPageAllocator {
    using StackType = Permutation<log2ceil(kernel::mm::PageAllocator::smallPagesPerBigPage)>;
    using SmallPageIndex = StackType::IndexType;
    StackType stack;
    SmallPageIndex occupiedStart;

    SmallPageAllocator(SmallPageIndex* fwb, SmallPageIndex* bwb);

    SmallPageAllocator(const SmallPageAllocator&) = delete;
    SmallPageAllocator& operator=(const SmallPageAllocator&) = delete;
    SmallPageAllocator(SmallPageAllocator&&) = delete;
    SmallPageAllocator& operator=(SmallPageAllocator&&) = delete;

    [[nodiscard]] constexpr bool allFree() const;
    [[nodiscard]] constexpr bool allFull() const;
    [[nodiscard]] constexpr size_t freePageCount() const;
    [[nodiscard]] SmallPageIndex allocateSmallPage();
    void freeSmallPage(SmallPageIndex index);
    void reserveSmallPage(SmallPageIndex index);
    void reserveAllPages();
    void freeAllPages();
};

using SmallPageIndex = SmallPageAllocator::SmallPageIndex;

struct BigPageMetadata {
    arch::InterruptDisablingPrioritySpinlock stealLock;
    BigPageState state;
    PoolID poolID;
    BigPageColor pageColor;

    BigPageMetadata* nextInPool;
    BigPageMetadata* prevInPool;

    BigPageMetadata* nextInColoredPool;
    BigPageMetadata* prevInColoredPool;

    SmallPageAllocator allocator;

    BigPageMetadata(SmallPageIndex* smallPageFwb, SmallPageIndex* smallPageBwb);

    BigPageMetadata(const BigPageMetadata&) = delete;
    BigPageMetadata& operator=(const BigPageMetadata&) = delete;
    BigPageMetadata(BigPageMetadata&&) = delete;
    BigPageMetadata& operator=(BigPageMetadata&&) = delete;

    [[nodiscard]] constexpr size_t freePageCount() const;
    SmallPageIndex allocateSmallPage();
    void freeSmallPage(SmallPageIndex index);
    void freeSmallPage(kernel::mm::phys_addr addr);
    void reserveSmallPage(SmallPageIndex index);
    void reserveAllSmallPages();
    void freeAllSmallPages();
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

struct AllocatorContext {
    BigPageMetadata* metadata;
    kernel::mm::phys_addr spanBase;
    PressureBitmap<PoolID> pressureBitmap;

    const size_t surplusThreshold;
    const size_t comfortThreshold;
    const size_t moderateThreshold;

    const size_t bigPageCount;

    AllocatorContext(kernel::mm::phys_memory_range allocatorRange, BootstrapAllocator& allocator);

    [[nodiscard]] kernel::mm::phys_addr bigPageAddress(const BigPageMetadata& m) const;
    [[nodiscard]] BigPageMetadata& bigPageForAddress(kernel::mm::phys_addr p);
};

enum class AllocationDesperation : uint8_t{
    RELAXED,
    MODERATE,
    DESPERATE
};

struct BigPagePool {
    arch::InterruptDisablingPrioritySpinlock lock;
    PoolID poolID;

    using BigPageList = IntrusiveLinkedList<BigPageMetadata, BigPageLinkedListExtractor>;
    using ColoredBigPageList = IntrusiveLinkedList<BigPageMetadata, BigPageColoredLinkedListExtractor>;

    BigPageList freeList;
    BigPageList fullList;
    BigPageList partialList;

    AllocatorContext& context;

    ColoredBigPageList coloredList[MAX_COLOR_COUNT + 1];

    size_t freeBigPageCount;
    size_t freeSmallPageCount;

    void addBigPage(BigPageMetadata& m);
    void removeBigPage(BigPageMetadata& m);
    BigPageMetadata* getPageForColoredSmallAllocation(BigPageColor color, size_t requestedCount, AllocationDesperation);
    size_t allocatePages(size_t requestedCount, PageAllocationCallback cb, BigPageColor color, AllocationDesperation desperation, BigPagePool& requestingPool);

    struct FreeResult {
        size_t stopIndex;
        size_t deferredFreeEnd;
    };

    //Attempts to free all pages starting from 'offset' in the 'pages' buffer. Stops as soon as it encounters a page
    //belonging to another pool. If desperation < DESPERATE, then we only make a best effort to acquire page-level
    //locks, but don't wait for them. If we fail to take a lock on that page, we move it deferredFreeEnd in page,
    //then increment deferredFreeEnd so that the buffer starts with any pages whose lock we could not acquire.
    [[nodiscard]] FreeResult freePages(PageRef* pages, size_t offset, size_t count, AllocationDesperation desperation);

    [[nodiscard]] PoolPressure computeUncoloredPressure() const;
    void updatePressureBitmap() const;

    BigPagePool(PoolID poolID, AllocatorContext& context);
    explicit BigPagePool(AllocatorContext& context);
    BigPagePool(const BigPagePool&) = delete;
    BigPagePool& operator=(const BigPagePool&) = delete;
    BigPagePool(BigPagePool&&) = delete;
    BigPagePool& operator=(BigPagePool&&) = delete;
};

using SmallPageBuff = SmallPageIndex[kernel::mm::PageAllocator::smallPagesPerBigPage];
struct alignas(arch::CACHE_LINE_SIZE) SmallPageAllocatorData {
    SmallPageBuff fwb;
    SmallPageBuff bwb;
} __attribute__((packed));

class RangeAllocator {
    AllocatorContext context;

    BigPagePool* localPools;
    BigPagePool globalPool;
    kernel::mm::phys_memory_range range;

    RangeAllocator* leftFreeChild;
    RangeAllocator* rightFreeChild;
    RangeAllocator* freeParent;
    kernel::mm::phys_memory_range subtreeRange;
    bool freeRed;

    friend struct RangeAllocatorFreeTreeExtractor;
    friend struct RangeAllocatorFreeTreeComparator;
    friend class AggregateAllocator;

public:
    RangeAllocator(kernel::mm::phys_memory_range range, BootstrapAllocator bootstrapAllocator);

    size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, Optional<BigPageColor> color = {});
    void freePages(PageRef* pages, size_t count);
};

struct RangeAllocatorFreeTreeExtractor {
    static RangeAllocator*& left(RangeAllocator& range){return range.leftFreeChild;}
    static RangeAllocator*& right(RangeAllocator& range){return range.rightFreeChild;}
    static RangeAllocator*& parent(RangeAllocator& range){return range.freeParent;}
    static RangeAllocator& data(RangeAllocator& range) {return range;}
    static RangeAllocator* const& left(const RangeAllocator& range){return range.leftFreeChild;}
    static RangeAllocator* const& right(const RangeAllocator& range){return range.rightFreeChild;}
    static RangeAllocator* const& parent(const RangeAllocator& range){return range.freeParent;}
    static const RangeAllocator& data(const RangeAllocator& range) {return range;}
    static bool isRed(RangeAllocator& range){return range.freeRed;}
    static void setRed(RangeAllocator& range, bool red){range.freeRed = red;}
    static kernel::mm::phys_memory_range& augmentedData(RangeAllocator& range){return range.subtreeRange;}
    static kernel::mm::phys_memory_range recomputeAugmentedData(const RangeAllocator& range, const RangeAllocator* left, const RangeAllocator* right) {
        auto laddr = range.range.start.value;
        auto raddr = range.range.end.value;
        if (left)
            laddr = left->subtreeRange.start.value;
        if (right)
            raddr = right->subtreeRange.end.value;
        return {kernel::mm::phys_addr(laddr), kernel::mm::phys_addr(raddr)};
    }
};

struct RangeAllocatorFreeTreeComparator {
    bool operator()(const RangeAllocator& r1, const RangeAllocator& r2) const{
        return r1.range.start.value < r2.range.start.value;
    }
};

//There is only one page allocator in the kernel. We're just wrapping this up in a class to make it easier
//to unit test. No global state to keep track of!
class AggregateAllocator {
    using FreeTree = IntrusiveRedBlackTree<RangeAllocator, RangeAllocatorFreeTreeExtractor, RangeAllocatorFreeTreeComparator>;

    FreeTree freeTree;
    PressureBitmap<size_t> rangePressures;
    Vector<RangeAllocator*> allocatorList;

    RangeAllocator* findAllocatorForPaddr(kernel::mm::phys_addr addr);
    bool markPageRuns(PageRef* pages, const size_t count);

public:
    AggregateAllocator(PressureBitmap<size_t>&& pressureBitmaps, Vector<RangeAllocator*>&& allocators);

    void freePages(PageRef* pages, const size_t count);
};

#endif //CROCOS_PAGEALLOCATOR_H