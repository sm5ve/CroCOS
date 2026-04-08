//
// Created by Spencer Martin on 2/4/26.
//

#include <mem/PageAllocator.h>
#include <core/math.h>
#include <core/ds/Trees.h>
#include <core/atomic/AtomicLinkedList.h>

constexpr size_t bigPagesInRange(const kernel::mm::phys_memory_range range) {
    const auto alignedTop = roundUpToNearestMultiple(range.end.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, static_cast<uint64_t>(arch::bigPageSize));
    return (alignedTop - alignedBottom)/arch::bigPageSize;
}

// ================= Bootstrap Allocator ==================

using namespace kernel;

BootstrapAllocator::BootstrapAllocator() : current(nullptr), end(nullptr), measuring(true) {}

BootstrapAllocator::BootstrapAllocator(void* buffer, size_t size)
        : current(static_cast<uint8_t *>(buffer)),
          end(static_cast<uint8_t *>(buffer) + size),
          measuring(false) {}

template<typename T>
T* BootstrapAllocator::allocate(const size_t count) {
    const size_t alignment = alignof(T);
    const auto addr = reinterpret_cast<size_t>(current);
    const size_t aligned = roundUpToNearestMultiple(addr, alignment);
    const size_t paddedObjSize = roundUpToNearestMultiple(sizeof(T), alignof(T));
    const size_t size = paddedObjSize * (count - 1) + sizeof(T);

    if (measuring) {
        current = reinterpret_cast<uint8_t *>(aligned + size);
        return nullptr;
    }

    current = reinterpret_cast<uint8_t *>(aligned);
    T* result = static_cast<T *>(static_cast<void*>(current));
    current += size;

    assert(current <= end, "Bootstrap allocator overflow");
    return result;
}

size_t BootstrapAllocator::bytesNeeded() const {
    return reinterpret_cast<size_t>(current);
}

size_t BootstrapAllocator::bytesRemaining() const {
    return measuring ? 0ul : static_cast<size_t>(end - current);
}

// ======================= PageRef ========================
mm::phys_addr PageRef::addr() const {
    return mm::phys_addr{value & ~(arch::smallPageSize - 1)};
}

mm::PageSize PageRef::size() const {
    return (value & 1) ? mm::PageSize::BIG : mm::PageSize::SMALL;
}

PageRef PageRef::small(mm::phys_addr addr) {
    assert(addr.value % arch::smallPageSize == 0, "Physical address is not small page aligned");
    return {addr.value};
}

PageRef PageRef::big(mm::phys_addr addr) {
    assert(addr.value % arch::bigPageSize == 0, "Physical address is not big page aligned");
    return {addr.value | 1};
}

static_assert(mm::PageAllocator::smallPagesPerBigPage * 2 <= arch::smallPageSize, "Can't smuggle run length in bottom of PageRef");

constexpr uint64_t pageRefRunMask = arch::smallPageSize - 2; //mask off all lower bits except for bottom

void PageRef::setRunLength(const size_t length) {
    assert(length <= mm::PageAllocator::smallPagesPerBigPage, "run length is too long");
    assert(length > 0, "run length must be nonzero");
    value &= ~pageRefRunMask;
    value |= (length - 1) << 1;
}

size_t PageRef::runLength() const {
    //Add 1 since the smallest run is of length 1.
    return ((value & pageRefRunMask) >> 1) + 1;
}

// ==================== SmallPageAllocator ====================
SmallPageAllocator::SmallPageAllocator(mm::phys_addr b) : ringBuffer(buffer, mm::PageAllocator::smallPagesPerBigPage), baseAddr(b){

}

void SmallPageAllocator::allocAll() {
    ringBuffer.clear();
    lazilyInitialized = mm::PageAllocator::smallPagesPerBigPage;
    for (auto& i : occupiedBitmap) {
        i = static_cast<uint64_t>(-1);
    }
}

void SmallPageAllocator::freeAll() {
    ringBuffer.clear();
    lazilyInitialized = 0;
    for (auto& i : occupiedBitmap) {
        i = 0;
    }
}

size_t SmallPageAllocator::alloc(PageAllocationCallback cb, size_t count) {
    SmallPageCount allocated = 0;

    // Try to allocate from uninitialized range
    while (allocated < count) {
        auto prevLazilyInitialized = lazilyInitialized.load(ACQUIRE);
        if (prevLazilyInitialized >= mm::PageAllocator::smallPagesPerBigPage) {
            break;  // No more uninitialized pages
        }

        // Claim a batch
        const size_t needed = count - allocated;
        const auto updatedValue = static_cast<SmallPageCount>(
            min(mm::PageAllocator::smallPagesPerBigPage,
                static_cast<size_t>(prevLazilyInitialized) + needed));

        if (!lazilyInitialized.compare_exchange(prevLazilyInitialized, updatedValue, RELEASE, ACQUIRE)) {
            continue;  // CAS failed, retry
        }

        // Process the claimed range [prevLazilyInitialized, updatedValue)
        for (SmallPageCount i = prevLazilyInitialized; i < updatedValue; i++) {
            if (isPageFree(i)) {  // Not reserved
                markPageFreeState(i, false);  // Mark as occupied
                cb(PageRef::small(fromPageIndex(i)));
                allocated++;
            }
            // If reserved, skip (it stays in occupied bitmap, we don't allocate it)
        }
    }

    // Fall back to ring buffer
    allocated += static_cast<SmallPageCount>(ringBuffer.bulkReadBestEffort(count - allocated, [&](auto _, SmallPageIndex index) {
        (void)_;
        markPageFreeState(index, false);
        cb(PageRef::small(fromPageIndex(index)));
    }));

    return allocated;
}

bool SmallPageAllocator::markPageFreeState(SmallPageIndex index, bool isFree) {
    constexpr auto bitmapWordSize = sizeof(occupiedBitmap[0]) * 8;
    auto wordIndex = divideAndRoundDown(static_cast<size_t>(index), bitmapWordSize);
    auto bitIndex = index % bitmapWordSize;
    auto bit = 1ull << bitIndex;
    uint64_t oldBitmapWord;
    if (isFree) {
        oldBitmapWord = occupiedBitmap[wordIndex].fetch_and(~bit, RELEASE);
    }
    else {
        oldBitmapWord = occupiedBitmap[wordIndex].fetch_or(bit, RELEASE);
    }
    return !((oldBitmapWord >> bitIndex) & 1);
}

bool SmallPageAllocator::isPageFree(SmallPageIndex index) const {
    constexpr auto bitmapWordSize = sizeof(occupiedBitmap[0]) * 8;
    auto wordIndex = divideAndRoundDown(static_cast<size_t>(index), bitmapWordSize);
    auto bitIndex = index % bitmapWordSize;
    return !((occupiedBitmap[wordIndex].load(ACQUIRE) >> bitIndex) & 1);
}

bool SmallPageAllocator::isPageFree(PageRef page) const {
    const auto addrRaw = page.addr().value;
    const SmallPageIndex index = (addrRaw / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage;
    return isPageFree(index);
}

void SmallPageAllocator::free(PageRef *pages, size_t count) {
    ringBuffer.bulkWrite(count, [&](auto index, auto& entry) {
        const auto addrRaw = pages[index].addr().value;
        assert((addrRaw & ~(arch::bigPageSize - 1)) == baseAddr.value, "Tried to free small page in wrong small page allocator");
        const SmallPageIndex pageIndex = (addrRaw / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage;
        const bool wasFree = markPageFreeState(pageIndex, true);
        assert(!wasFree, "Double free: page is already free");
        entry = pageIndex;
    });
}

size_t SmallPageAllocator::freePageCount() const {
    size_t out = mm::PageAllocator::smallPagesPerBigPage - lazilyInitialized.load(RELAXED);
    out -= reservedCount;
    out += ringBuffer.availableToRead();
    return out;
}

bool SmallPageAllocator::isEmpty() const {
    return freePageCount() == mm::PageAllocator::smallPagesPerBigPage;
}

bool SmallPageAllocator::isFull() const {
    return freePageCount() == 0;
}

mm::phys_addr SmallPageAllocator::fromPageIndex(SmallPageIndex index) const {
    return baseAddr + index * arch::smallPageSize;
}

void SmallPageAllocator::reservePage(kernel::mm::phys_addr addr) {
    assert(lazilyInitialized.load(RELAXED) == 0, "Can only reserve pages during memory allocator init");
    assert(addr.value % arch::smallPageSize == 0, "Address must be page aligned");
    SmallPageIndex index = (addr.value / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage;
    reservedCount++;
    markPageFreeState(index, false);
}

// ==================== BigPageMetadata ====================

size_t BigPageMetadata::allocatePages(size_t smallPageCount, PageAllocationCallback cb) {
    return subpageAllocator.alloc(cb, smallPageCount);
}

void BigPageMetadata::freePages(PageRef *pages, size_t count) {
    subpageAllocator.free(pages, count);
}

bool BigPageMetadata::isEmpty() const {
    return subpageAllocator.isEmpty();
}

bool BigPageMetadata::isFull() const {
    return subpageAllocator.isFull();
}
