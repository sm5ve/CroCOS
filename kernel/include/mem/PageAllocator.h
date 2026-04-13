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
#include <core/atomic/AtomicBitPool.h>

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
    T* allocate(size_t count, size_t alignment = alignof(T));
    template<typename T>
    T* allocate();
    template<typename T>
    T* allocate(FunctionRef<void(T&)> init, size_t count, size_t alignment = alignof(T));
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
    friend class BigPageMetadata;
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
    SmallPageAllocator subpageAllocator;
    NUMAPool& ownerPool;

public:
    BigPageMetadata(NUMAPool& pool, kernel::mm::phys_addr baseAddr);

    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb);
    void freePages(PageRef* pages, size_t count);

    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;

    [[nodiscard]] NUMAPool& getOwnerPool() const { return ownerPool; }
};

struct SubrangeInfo {
    kernel::mm::phys_memory_range base;
    size_t bitPoolOffset;
};

class NUMAPool {
    BigPageMetadata* bigPageMetadataBuffer;
    MPMCRingBuffer<BigPageMetadata*, false, true> freeBigPages;
    AtomicBitPool paPages;
    kernel::numa::DomainID associatedDomain;
    SubrangeInfo* subrangeInfo;
    size_t subrangeCount;
public:
    NUMAPool(const Vector<kernel::mm::phys_memory_range>& memoryRanges, void* bufferStart, size_t bufferSize);
};

class LocalPool {

};

// ==================== New Page Allocator ====================

struct PageAllocatorImpl {
    // TODO: full implementation TBD
};

NUMAPool*         createNumaPool(BootstrapAllocator& alloc, const Vector<kernel::mm::phys_memory_range>& ranges);
LocalPool*        createLocalPool(BootstrapAllocator& alloc);
PageAllocatorImpl createPageAllocator(Vector<NUMAPool*>&& perDomainAllocs, LocalPool** localPools);

#endif //CROCOS_PAGEALLOCATOR_H