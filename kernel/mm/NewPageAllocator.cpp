//
// Created by Spencer Martin on 2/4/26.
//

#include <mem/PageAllocator.h>
#include <core/math.h>

/*
 * This file implements the page allocator, which supports two allocation sizes:
 * small pages (4KB on amd64) and big pages (2MB on amd64). Big pages are "owned"
 * by pools (global or CPU-local) and contain small page allocators. Allocations
 * may be "colored" to group related allocations and reduce fragmentation. Big pages
 * may be donated to the global pool or stolen between pools, preferring LRU pages.
 *
 * LOCKING PROTOCOL:
 * To minimize lock contention and prevent deadlocks, the following rules govern
 * lock acquisition:
 *
 * 1. Lock types: Big page locks and pool locks.
 *
 * 2. Blocking lock acquisition is permitted only when:
 *    - Holding no locks (can wait for any lock)
 *    - Holding only a big page lock (can wait for that page's pool lock)
 *
 * 3. Non-blocking acquisition (try-lock) is required in all other circumstances
 *
 * 4. Big pages "in transit" (removed from source pool, not yet in destination)
 *    may be locked while holding either source or destination pool lock.
 *
 * 5. CRITICAL: At most ONE big page from a given pool may be locked without
 *    holding that pool's lock. This prevents deadlock when multiple CPUs
 *    attempt multiple bigPage acquisitions.
 *
 * 6. Interrupts must be disabled whenever any lock is held. For batch operations
 *    (removing multiple pages), keep interrupts disabled throughout.
 *
 * 7. PERFORMANCE: To reduce pool lock hold time, prefer this pattern:
 *    - Lock pool, remove pages, unlock pool
 *    - Process pages (free/allocate) without pool lock
 *    - Lock pool, merge results back, unlock pool
 */

// ================= Bootstrap Allocator ==================

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
    const size_t size = sizeof(T) * count;

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

//For eventual use in higher-level allocation policies
bool tryAcquireLock(arch::InterruptDisablingSpinlock& spinlock, size_t retryIterations = LOCK_RETRY_COUNT, size_t delayCount = LOCK_DELAY_ITERATIONS) {
    for (size_t i = 0; i < retryIterations; i++) {
        if (spinlock.tryAcquire()) {
            return true;
        }
        for (size_t j = 0; j < delayCount; j++) {
            tight_spin();
        }
    }
    return false;
}

// ==================== PoolID ====================

bool PoolID::global() const {
    return id == static_cast<IDType>(-1);
}

// ==================== SmallPageAllocator ====================

SmallPageAllocator::SmallPageAllocator(SmallPageIndex* fwb, SmallPageIndex* bwb)
    : stack(fwb, bwb, mm::PageAllocator::smallPagesPerBigPage) {
    occupiedStart = mm::PageAllocator::smallPagesPerBigPage;
    stack.reset();
}

constexpr bool SmallPageAllocator::allFree() const {
    return occupiedStart == mm::PageAllocator::smallPagesPerBigPage;
}

constexpr bool SmallPageAllocator::allFull() const {
    return occupiedStart == 0;
}

constexpr size_t SmallPageAllocator::freePageCount() const {
    return occupiedStart;
}

SmallPageIndex SmallPageAllocator::allocateSmallPage() {
    assert(freePageCount() > 0, "Tried to allocate small page from totally full big page");
    occupiedStart--;
    return stack[StackType::PositionIndex{occupiedStart}];
}

void SmallPageAllocator::freeSmallPage(const SmallPageIndex index) {
    const StackType::ElementIndex elem{index};
    assert(stack.positionOf(elem) >= occupiedStart, "Tried to double-free small page");
    stack.swap(elem, StackType::PositionIndex{occupiedStart});
    occupiedStart++;
}

void SmallPageAllocator::reserveSmallPage(const SmallPageIndex index) {
    occupiedStart--;
    stack.swap(StackType::PositionIndex{occupiedStart}, StackType::PositionIndex{index});
}

void SmallPageAllocator::reserveAllPages() {
    occupiedStart = 0;
}

// ==================== BigPageMetadata ====================

BigPageMetadata::BigPageMetadata(SmallPageIndex* smallPageFwb, SmallPageIndex* smallPageBwb)
    : allocator(smallPageFwb, smallPageBwb) {
    state = BigPageState::FREE;
    poolID = GLOBAL;
    pageColor = uncolored;
    nextInPool = nullptr;
    prevInPool = nullptr;
    nextInColoredPool = nullptr;
    prevInColoredPool = nullptr;
}

constexpr size_t BigPageMetadata::freePageCount() const {
    return allocator.freePageCount();
}

SmallPageIndex BigPageMetadata::allocateSmallPage() {
    auto out = allocator.allocateSmallPage();
    if (allocator.allFull()) {
        state = BigPageState::FULL;
    }
    else {
        state = BigPageState::PARTIALLY_ALLOCATED;
    }
    return out;
}

void BigPageMetadata::freeSmallPage(const SmallPageIndex index) {
    allocator.freeSmallPage(index);
    if (allocator.allFree()) {
        state = BigPageState::FREE;
    }
    else {
        state = BigPageState::PARTIALLY_ALLOCATED;
    }
}

void BigPageMetadata::reserveAllSmallPages() {
    allocator.reserveAllPages();
    state = BigPageState::FULL;
}

void BigPageMetadata::reserveSmallPage(const SmallPageIndex index) {
    allocator.reserveSmallPage(index);
    if (allocator.allFull()) {
        state = BigPageState::FULL;
    }
    else {
        state = BigPageState::PARTIALLY_ALLOCATED;
    }
}

// ==================== BigPageLinkedListExtractor ====================

BigPageMetadata*& BigPageLinkedListExtractor::previous(BigPageMetadata& m) {
    return m.prevInPool;
}

BigPageMetadata*& BigPageLinkedListExtractor::next(BigPageMetadata& m) {
    return m.nextInPool;
}

// ==================== BigPageColoredLinkedListExtractor ====================

BigPageMetadata*& BigPageColoredLinkedListExtractor::previous(BigPageMetadata& m) {
    return m.prevInColoredPool;
}

BigPageMetadata*& BigPageColoredLinkedListExtractor::next(BigPageMetadata& m) {
    return m.nextInColoredPool;
}

// ==================== AllocatorContext ====================

AllocatorContext::AllocatorContext(mm::phys_memory_range allocatorRange, BigPageMetadata *bigPageBuffer) {
    metadata = bigPageBuffer;
    spanBase = mm::phys_addr{roundDownToNearestMultiple(allocatorRange.start.value, arch::bigPageSize)};
}

mm::phys_addr AllocatorContext::bigPageAddress(const BigPageMetadata& m) const {
    const auto metaBase = reinterpret_cast<size_t>(metadata);
    const auto metaEntry = reinterpret_cast<size_t>(&m);
    const auto index = (metaEntry - metaBase) / sizeof(BigPageMetadata);
    return spanBase + index * arch::bigPageSize;
}

// ==================== BigPagePool ====================

BigPagePool::BigPagePool(AllocatorContext& ctx) : context(ctx) {
    freeSmallPageCount = 0;
    freeBigPageCount = 0;
}

BigPagePool::BigPagePool(const PoolID pid, AllocatorContext& ctx) : context(ctx) {
    freeSmallPageCount = 0;
    freeBigPageCount = 0;
    poolID = pid;
}

void BigPagePool::addBigPage(BigPageMetadata& m) {
    assert(lock.lockTaken(), "Can't modify pool without acquiring lock first");
    switch (m.state) {
        case BigPageState::FREE:
            assert(m.allocator.allFree(), "Big page claims to be completely free, small page allocator has allocations");
            freeList.pushFront(m);
            freeBigPageCount++;
            break;
        case BigPageState::FULL:
            fullList.pushFront(m);
            break;
        case BigPageState::PARTIALLY_ALLOCATED:
            partialList.pushFront(m);
            coloredList[m.pageColor].pushFront(m);
            freeSmallPageCount += m.freePageCount();
            break;
    }
    m.poolID = poolID;
}

void BigPagePool::removeBigPage(BigPageMetadata& m) {
    assert(lock.lockTaken(), "Can't modify pool without acquiring lock first");
    switch (m.state) {
        case BigPageState::FREE:
            freeList.remove(m);
            freeBigPageCount--;
            break;
        case BigPageState::FULL:
            fullList.remove(m);
            break;
        case BigPageState::PARTIALLY_ALLOCATED:
            partialList.remove(m);
            coloredList[m.pageColor].remove(m);
            freeSmallPageCount -= m.freePageCount();
            break;
    }
}

BigPageMetadata* BigPagePool::getPageForColoredSmallAllocation(BigPageColor color, size_t requestedCount, AllocationDesperation desperation) {
    assert(lock.lockTaken(), "Can't modify pool without acquiring lock first");

    if (!coloredList[color].empty()) {
        const auto out = coloredList[color].head();
        removeBigPage(*out);
        return out;
    }
    if (!coloredList[uncolored].empty()) {
        const auto out = coloredList[uncolored].head();
        removeBigPage(*out);
        return out;
    }
    //If we need a lot of small pages relative to the number of free small pages, get a fresh page
    if (requestedCount * 4 > freeSmallPageCount && !freeList.empty()) {
        const auto out = freeList.head();
        removeBigPage(*out);
        out->pageColor = color;
        return out;
    }

    if (desperation < AllocationDesperation::MODERATE) {
        return nullptr;
    }

    //If we don't have a lot of free pages to spare, try to allocate from a partially allocated page of the wrong color
    if (freeBigPageCount < LOCAL_POOL_FREE_COMFORT_THRESHOLD) {
        //TODO if we're not desperate, let's look for a partially occupied page that has a lot of free space
        if (!partialList.empty()) {
            const auto out = partialList.head();
            removeBigPage(*out);
            out->pageColor = uncolored;
            return out;
        }
    }

    if (!freeList.empty()) {
        const auto out = freeList.head();
        removeBigPage(*out);
        out->pageColor = color;
        return out;
    }

    return nullptr;
}

size_t BigPagePool::allocatePages(size_t requestedCount, PageAllocationCallback cb, BigPageColor color, AllocationDesperation desperation, BigPagePool& requestingPool) {
    const bool localPool = (&requestingPool == this);
    if (localPool) {
        //If we're allocating from this pool's corresponding CPU, we always want to prioritize this pool above others.
        //Therefore, we always wait to acquire the lock.
        lock.acquirePriority();
    }
    else {
        if (desperation < AllocationDesperation::DESPERATE) {
            if (!lock.tryAcquire()) {
                return 0;
            }
        }
        else {
            lock.acquire();
        }
    }

    IntrusiveLinkedList<BigPageMetadata, BigPageLinkedListExtractor> toProcessList;
    IntrusiveLinkedList<BigPageMetadata, BigPageLinkedListExtractor> failedProcessList;
    IntrusiveLinkedList<BigPageMetadata, BigPageLinkedListExtractor> completedList;
    size_t accommodatedCount = 0;
    while (auto* bigPage = getPageForColoredSmallAllocation(color, requestedCount - accommodatedCount, desperation)) {
        //We don't yet need to acquire a lock on bigPage since the page cannot be used for further allocations once
        //it's removed from a pool. It can only be freed from.
        toProcessList.pushBack(*bigPage);
        accommodatedCount += bigPage->freePageCount();
        if (accommodatedCount >= requestedCount) {
            break;
        }
    }
    //Unlock the pool while we actually allocate the pages.

    arch::InterruptResetter resetter;
    if (localPool) {
        resetter = lock.releasePriorityPlain();
    }
    else {
        resetter = lock.releasePlain();
    }

    size_t allocatedCount = 0;
    const auto allocateFromBigPage = [&](BigPageMetadata& bigPage) {
        auto bigPageAddr = context.bigPageAddress(bigPage);
        completedList.pushBack(bigPage);
        //If the big page happens to be completely free and we still need at least a big page's worth of memory,
        //then allocate it all at once.
        if (bigPage.allocator.allFree() && (requestedCount - allocatedCount) >= mm::PageAllocator::smallPagesPerBigPage) {
            cb(mm::PageSize::BIG, bigPageAddr);
            allocatedCount += mm::PageAllocator::smallPagesPerBigPage;
            bigPage.reserveAllSmallPages();
            return;
        }
        //Otherwise just allocate one page at a time.
        while ((allocatedCount < requestedCount) && !(bigPage.allocator.allFull())) {
            const auto pageIndex = bigPage.allocateSmallPage();
            const auto pageAddr = bigPageAddr + pageIndex * arch::smallPageSize;
            cb(mm::PageSize::SMALL, pageAddr);
            allocatedCount++;
        }
    };

    while (auto* bigPage = toProcessList.popFront()) {
        if (allocatedCount >= requestedCount) break;
        //If we can't acquire the lock first try, we'll put it in a queue to try again later
        if (!bigPage -> stealLock.tryAcquire()) {
            failedProcessList.pushBack(*bigPage);
            continue;
        }
        allocateFromBigPage(*bigPage);
    }

    //For pages where we couldn't acquire the lock first try, we'll commit to more drastic action
    while (auto* bigPage = failedProcessList.popFront()) {
        if (allocatedCount >= requestedCount) break;

        bigPage -> stealLock.acquirePriority();
        allocateFromBigPage(*bigPage);
    }

    //TODO when returning pages to the relevant pools, we may want to pre-sort them into appropriate linked lists
    //Then just do a single splicing of the linked lists.

    //It is possible that we failed to process a page on the first go, and for a few reasons we didn't need to
    //allocate *any* memory from that page. If we were originally going to steal that page to a different pool,
    //we'll make an effort to return it to its original pool (this) if we can acquire the lock
    if (!(failedProcessList.empty() || localPool)) {
        if (lock.tryAcquire()) {
            while (auto* bigPage = failedProcessList.popFront()) {
                addBigPage(*bigPage);
                bigPage -> stealLock.release();
            }
            //Under strange circumstances, it is possible that toProcessList may not be empty
            //for instance if a page in that list was largely full at selection time, but
            //another CPU freed a bunch of small pages
            while (auto* bigPage = toProcessList.popFront()) {
                addBigPage(*bigPage);
                bigPage -> stealLock.release();
            }
            lock.release();
        }
    }

    //Finally put all the remaining pages in the caller's pool
    requestingPool.lock.acquirePriority();
    while (auto* bigPage = failedProcessList.popFront()) {
        requestingPool.addBigPage(*bigPage);
        bigPage -> stealLock.release();
    }
    while (auto* bigPage = toProcessList.popFront()) {
        requestingPool.addBigPage(*bigPage);
        bigPage -> stealLock.release();
    }
    while (auto* bigPage = completedList.popFront()) {
        requestingPool.addBigPage(*bigPage);
        bigPage -> stealLock.release();
    }
    requestingPool.lock.releasePriority();

    resetter();

    return allocatedCount;
}

// ==================== PressureBitmap ====================

void PressureBitmap::measureAllocation(BootstrapAllocator& allocator, size_t processorCount) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t bitmapWords = divideAndRoundUp(processorCount + 1, BITS_PER_WORD);
    // Measure 4 bitmaps (one per pressure level), each with bitmapWords words
    allocator.allocate<Atomic<uint64_t>>(bitmapWords * static_cast<size_t>(PoolPressure::COUNT));
}

PressureBitmap::PressureBitmap(BootstrapAllocator& allocator, size_t processorCount) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t bitmapWords = divideAndRoundUp(processorCount + 1, BITS_PER_WORD);
    for (auto& bitmap : bitmaps) {
        bitmap = allocator.allocate<Atomic<uint64_t>>(bitmapWords);
        // Initialize all words to 0
        for (size_t j = 0; j < bitmapWords; j++) {
            bitmap[j].store(0, RELAXED);
        }
    }
}

void PressureBitmap::markPressure(PoolID pool, PoolPressure pressure) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t totalBits = arch::processorCount() + 1;
    const size_t bitIndex = pool.global() ? totalBits - 1 : pool.id;
    const size_t wordIndex = bitIndex / BITS_PER_WORD;
    const uint64_t bitMask = 1ULL << (bitIndex % BITS_PER_WORD);

    // First set bit in target pressure level
    bitmaps[static_cast<size_t>(pressure)][wordIndex].fetch_or(bitMask, RELAXED);

    // Then clear bit from all other pressure levels
    for (size_t i = 0; i < static_cast<size_t>(PoolPressure::COUNT); i++) {
        if (i != static_cast<size_t>(pressure))
            bitmaps[i][wordIndex].fetch_and(~bitMask, RELAXED);
    }
}

IteratorRange<PressureBitmap::BitmapIterator> PressureBitmap::poolsWithPressure(PoolPressure pressure) const {
    const size_t totalBits = arch::processorCount() + 1;
    Atomic<uint64_t>* bitmap = bitmaps[static_cast<size_t>(pressure)];

    return {BitmapIterator(bitmap, 0, totalBits), BitmapIterator(bitmap, totalBits, totalBits)};
}

PressureBitmap::BitmapIterator::BitmapIterator(Atomic<uint64_t>* bitmap, size_t idx, size_t total)
    : bitmapStart(bitmap), totalBits(total), index(idx) {
#ifdef PA_BITMAP_ITERATOR_CACHE_WORD
    currentWord = 0;
#endif
    // Only advance if not at the end
    if (index < totalBits) {
        advanceToSetBit();
    }
}

void PressureBitmap::BitmapIterator::advanceToSetBit() {
    constexpr size_t BITS_PER_WORD = 64;

#ifdef PA_BITMAP_ITERATOR_CACHE_WORD
    while (index < totalBits) {
        size_t wordIndex = index / BITS_PER_WORD;
        size_t bitOffset = index % BITS_PER_WORD;

        // If we're at the start of a new word, load it
        if (bitOffset == 0) {
            currentWord = bitmapStart[wordIndex].load(RELAXED);
        }

        // Mask off bits before our current position
        uint64_t maskedWord = currentWord & ~((1ULL << bitOffset) - 1);

        if (maskedWord != 0) {
            // Found a set bit
            size_t bitPosition = countTrailingZeros(maskedWord);
            index = wordIndex * BITS_PER_WORD + bitPosition;
            return;
        }

        // No set bits in remainder of this word, move to next word
        index = (wordIndex + 1) * BITS_PER_WORD;
    }
#else
    while (index < totalBits) {
        size_t wordIndex = index / BITS_PER_WORD;
        size_t bitOffset = index % BITS_PER_WORD;

        uint64_t word = bitmapStart[wordIndex].load(MemoryOrder::Relaxed);

        // Mask off bits before our current position
        uint64_t maskedWord = word & ~((1ULL << bitOffset) - 1);

        if (maskedWord != 0) {
            // Found a set bit
            size_t bitPosition = countTrailingZeros(maskedWord);
            index = wordIndex * BITS_PER_WORD + bitPosition;
            return;
        }

        // No set bits in remainder of this word, move to next word
        index = (wordIndex + 1) * BITS_PER_WORD;
    }
#endif
}

bool PressureBitmap::BitmapIterator::atEnd() const {
    return index >= totalBits;
}

PressureBitmap::BitmapIterator& PressureBitmap::BitmapIterator::operator++() {
    if (index < totalBits) {
        index++;
        advanceToSetBit();
    }
    return *this;
}

bool PressureBitmap::BitmapIterator::operator==(const BitmapIterator& other) const {
    return index == other.index && bitmapStart == other.bitmapStart;
}

PoolID PressureBitmap::BitmapIterator::operator*() const {
    if (index == totalBits - 1) {
        return GLOBAL;
    }
    return {static_cast<arch::ProcessorID>(index)};
}

// ==================== Range Allocator ===================

constexpr size_t bigPagesInRange(const mm::phys_memory_range range) {
    const auto alignedTop = roundUpToNearestMultiple(range.end.value, arch::bigPageSize);
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, arch::bigPageSize);
    return (alignedTop - alignedBottom)/arch::bigPageSize;
}

RangeAllocator::RangeAllocator(const mm::phys_memory_range range, BootstrapAllocator bootstrapAllocator) :
context(range, bootstrapAllocator.allocate<BigPageMetadata>(bigPagesInRange(range))),
globalPool(GLOBAL, context){
    assert(!bootstrapAllocator.isFake(), "The bootstrap allocator cannot be in measurement mode");
    assert(range.start.value % arch::smallPageSize == 0, "Range allocator start is not page aligned");
    assert(range.end.value % arch::smallPageSize == 0, "Range allocator end is not page aligned");

    const auto alignedTop = roundUpToNearestMultiple(range.end.value, arch::bigPageSize);
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, arch::bigPageSize);
    const auto bottomReserveCount = (range.start.value - alignedBottom) / arch::smallPageSize;
    const auto topReserveCount = (range.end.value - alignedTop) / arch::smallPageSize;

    const size_t bigPageCount = (alignedTop - alignedBottom) / arch::bigPageSize;

    auto* smallPageAllocatorMemory = bootstrapAllocator.allocate<SmallPageAllocatorData>(bigPageCount);
    auto* bigPageBuffer = context.metadata;
    localPools = bootstrapAllocator.allocate<BigPagePool>(arch::processorCount());

    globalPool.lock.acquire();
    for (size_t i = 0; i < bigPageCount; i++) {
        auto&[fwb, bwb] = smallPageAllocatorMemory[i];
        new(&bigPageBuffer[i])BigPageMetadata(fwb, bwb);
        if (i == 0) {
            for (size_t j = 0; j < bottomReserveCount; j++) {
                bigPageBuffer[i].reserveSmallPage(static_cast<SmallPageIndex>(j));
            }
        }
        if (i == bigPageCount - 1) {
            for (size_t j = 0; j < topReserveCount; j++) {
                const auto ind = mm::PageAllocator::smallPagesPerBigPage - j - 1;
                bigPageBuffer[i].reserveSmallPage(static_cast<SmallPageIndex>(ind));
            }
        }
        globalPool.addBigPage(bigPageBuffer[i]);
    }
    globalPool.lock.release();

    for (size_t i = 0; i < arch::processorCount(); i++) {
        new(&localPools[i])BigPagePool(static_cast<arch::ProcessorID>(i), context);
    }
}

//size_t RangeAllocator::allocatePages(size_t smallPageCount, PageAllocationCallback cb, Optional<BigPageColor> color) {

//}

// ==================== Bootstrap Code ====================



