//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATOR_H
#define CROCOS_PAGEALLOCATOR_H

#include <kernel.h>
#include <arch.h>
#include <core/utility.h>
#include <core/atomic.h>
#include <core/ds/permutation.h>
#include <core/ds/LinkedList.h>
#include <mem/PageAllocatorTuning.h>
#include <core/Iterator.h>

#include <mem/mm.h>

#define PA_BITMAP_ITERATOR_CACHE_WORD

using PageAllocationCallback = FunctionRef<void(kernel::mm::PageSize, kernel::mm::phys_addr)>;

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
    void reserveSmallPage(SmallPageIndex index);
    void reserveAllSmallPages();
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

class PressureBitmap {
    Atomic<uint64_t>* bitmaps[static_cast<size_t>(PoolPressure::COUNT)];
    size_t processorCount;
public:
    class BitmapIterator {
        Atomic<uint64_t>* bitmapStart;
        const size_t totalBits;
#ifdef PA_BITMAP_ITERATOR_CACHE_WORD
        uint64_t currentWord;
#endif
        size_t index;

        void advanceToSetBit();

    public:
        BitmapIterator(Atomic<uint64_t>* bitmap, size_t index, size_t totalBits);
        [[nodiscard]] bool atEnd() const;
        BitmapIterator& operator++();
        bool operator==(const BitmapIterator& other) const;
        PoolID operator*() const;
    };

    static void measureAllocation(BootstrapAllocator& allocator, size_t processorCount);
    PressureBitmap(BootstrapAllocator& allocator, size_t processorCount);

    void markPressure(PoolID pool, PoolPressure pressure);

    IteratorRange<BitmapIterator> poolsWithPressure(PoolPressure pressure) const;
};

struct AllocatorContext {
    BigPageMetadata* metadata;
    kernel::mm::phys_addr spanBase;
    PressureBitmap pressureBitmap;

    const size_t surplusThreshold;
    const size_t comfortThreshold;
    const size_t moderateThreshold;

    AllocatorContext(kernel::mm::phys_memory_range allocatorRange, BootstrapAllocator& allocator);

    [[nodiscard]] kernel::mm::phys_addr bigPageAddress(const BigPageMetadata& m) const;
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
public:
    RangeAllocator(kernel::mm::phys_memory_range range, BootstrapAllocator bootstrapAllocator);

    size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, Optional<BigPageColor> color = {});
};

#endif //CROCOS_PAGEALLOCATOR_H