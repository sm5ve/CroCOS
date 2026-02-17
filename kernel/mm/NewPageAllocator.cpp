//
// Created by Spencer Martin on 2/4/26.
//

#include <mem/PageAllocator.h>
#include <core/math.h>
#include <core/ds/Trees.h>

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
bool tryAcquireLock(arch::InterruptDisablingPrioritySpinlock& spinlock, size_t retryIterations = LOCK_RETRY_COUNT, size_t delayCount = LOCK_DELAY_ITERATIONS) {
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

void SmallPageAllocator::freeAllPages() {
    occupiedStart = mm::PageAllocator::smallPagesPerBigPage;
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

void BigPageMetadata::freeSmallPage(const mm::phys_addr addr) {
    const auto bigPageOffset = addr.value % arch::bigPageSize;
    assert(bigPageOffset % arch::smallPageSize == 0, "Address must be small page aligned");
    const auto index = static_cast<SmallPageIndex>(bigPageOffset / arch::smallPageSize);
    freeSmallPage(index);
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

void BigPageMetadata::freeAllSmallPages() {
    allocator.freeAllPages();
    state = BigPageState::FREE;
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

constexpr size_t bigPagesInRange(const mm::phys_memory_range range) {
    const auto alignedTop = roundUpToNearestMultiple(range.end.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, static_cast<uint64_t>(arch::bigPageSize));
    return (alignedTop - alignedBottom)/arch::bigPageSize;
}

constexpr size_t computeModerateThreshold(mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/8, MODERATE_THRESHOLD_MINIMUM);
}

constexpr size_t computeComfortableThreshold(mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/4, MODERATE_THRESHOLD_MINIMUM * 2);
}

constexpr size_t computeSurplusThreshold(mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/2, MODERATE_THRESHOLD_MINIMUM * 4);
}

AllocatorContext::AllocatorContext(mm::phys_memory_range allocatorRange, BootstrapAllocator& allocator) :
    pressureBitmap(allocator, arch::processorCount()),
    surplusThreshold(computeSurplusThreshold(allocatorRange, arch::processorCount())),
    comfortThreshold(computeComfortableThreshold(allocatorRange, arch::processorCount())),
    moderateThreshold(computeModerateThreshold(allocatorRange, arch::processorCount())),
    bigPageCount(bigPagesInRange(allocatorRange)){
    metadata = allocator.allocate<BigPageMetadata>(bigPagesInRange(allocatorRange));
    spanBase = mm::phys_addr{
        roundDownToNearestMultiple(allocatorRange.start.value, static_cast<uint64_t>(arch::bigPageSize))
    };
}

mm::phys_addr AllocatorContext::bigPageAddress(const BigPageMetadata& m) const {
    const auto metaBase = reinterpret_cast<size_t>(metadata);
    const auto metaEntry = reinterpret_cast<size_t>(&m);
    const auto index = (metaEntry - metaBase) / sizeof(BigPageMetadata);
    return spanBase + index * arch::bigPageSize;
}

BigPageMetadata& AllocatorContext::bigPageForAddress(const mm::phys_addr addr) {
    const auto bigPageAddr = roundDownToNearestMultiple(addr.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto index = (bigPageAddr - spanBase.value) / arch::bigPageSize;
    return metadata[index];
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
    updatePressureBitmap();
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
    updatePressureBitmap();
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
    //TODO respect MAX_BATCH_SIZE
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
    //Notably, this should allow us to do some controlled reentrancy where the callback requires a further page allocation!
    //We will need to unit-test this.
    //This reentrancy is permissible under the locking rules since we remove the big pages from the pool before acquiring
    //their locks, thus we do not violate rule 5
    arch::InterruptResetter resetter = localPool ? lock.releasePriorityPlain() : lock.releasePlain();
    defer(resetter);

    size_t allocatedCount = 0;
    const auto allocateFromBigPage = [&](BigPageMetadata& bigPage) {
        auto bigPageAddr = context.bigPageAddress(bigPage);
        completedList.pushBack(bigPage);
        //If the big page happens to be completely free and we still need at least a big page's worth of memory,
        //then allocate it all at once.
        if (bigPage.allocator.allFree() && (requestedCount - allocatedCount) >= mm::PageAllocator::smallPagesPerBigPage) {
            cb(PageRef::big(bigPageAddr));
            allocatedCount += mm::PageAllocator::smallPagesPerBigPage;
            bigPage.reserveAllSmallPages();
            return;
        }
        //Otherwise just allocate one page at a time.
        while ((allocatedCount < requestedCount) && !(bigPage.allocator.allFull())) {
            const auto pageIndex = bigPage.allocateSmallPage();
            const auto pageAddr = bigPageAddr + pageIndex * arch::smallPageSize;
            cb(PageRef::small(pageAddr));
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

    return allocatedCount;
}

PoolPressure BigPagePool::computeUncoloredPressure() const {
    //We weight small pages less than big pages, because we want fragmentation to contribute to the recorded pressure
    //of the pool. These parameters should be tuned at some point.
    const auto effectiveBigPages = freeBigPageCount + (freeSmallPageCount * SMALL_PAGE_WEIGHT_NUM) / (mm::PageAllocator::smallPagesPerBigPage * SMALL_PAGE_WEIGHT_DEN);
    if (effectiveBigPages >= context.surplusThreshold) {
        return PoolPressure::SURPLUS;
    }
    if (effectiveBigPages >= context.comfortThreshold) {
        return PoolPressure::COMFORTABLE;
    }
    if (effectiveBigPages >= context.moderateThreshold) {
        return PoolPressure::MODERATE;
    }
    return PoolPressure::DESPERATE;
}

void BigPagePool::updatePressureBitmap() const {
    const auto newPressure = computeUncoloredPressure();
    context.pressureBitmap.markPressure(poolID, newPressure);
}

/*BigPagePool::FreeResult BigPagePool::freePages(PageRef* pages, const size_t offset, const size_t count, const AllocationDesperation desperation) {
    const bool localPool = (poolID == PoolID{arch::getCurrentProcessorID()});
    size_t deferredFreeEnd = 0;
    bool acquiredPriority = false;

    auto acquirePoolLock = [&] {
        if (localPool) {
            lock.acquirePriority();
            acquiredPriority = true;
            return true;
        }
        switch (desperation) {
            case AllocationDesperation::RELAXED:
                if (!lock.tryAcquire()) {
                    // Can't acquire pool lock at all - everything is deferred
                    return false;
                }
                acquiredPriority = false;
            case AllocationDesperation::MODERATE:
                //Retry a few times.
                if (!tryAcquireLock(lock)) {
                    return false;
                }
                acquiredPriority = false;
            case AllocationDesperation::DESPERATE:
                assertNotReached("This method should not be called in DESPERATE mode");
        }
        return true;
    };

    auto releasePoolLock = [&] () {
        if (acquiredPriority) {
            return lock.releasePriorityPlain();
        }
        return lock.releasePlain();
    };

    BigPageMetadata* freeBatch[MAX_FREE_BATCH_SIZE];

}*/

// ==================== PressureBitmap ====================

template<typename IndexType>
void PressureBitmap<IndexType>::measureAllocation(BootstrapAllocator& allocator, size_t entryCount) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t requiredBits = Traits::requiredBits(entryCount);
    const size_t bitmapWords = divideAndRoundUp(requiredBits, BITS_PER_WORD);
    // Measure 4 bitmaps (one per pressure level), each with bitmapWords words
    allocator.allocate<Atomic<uint64_t>>(bitmapWords * static_cast<size_t>(PoolPressure::COUNT));
}

template<typename IndexType>
PressureBitmap<IndexType>::PressureBitmap(BootstrapAllocator& allocator, size_t count)
    : entryCount(count) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t requiredBits = Traits::requiredBits(entryCount);
    const size_t bitmapWords = divideAndRoundUp(requiredBits, BITS_PER_WORD);
    for (auto& bitmap : bitmaps) {
        bitmap = allocator.allocate<Atomic<uint64_t>>(bitmapWords);
        // Initialize all words to 0
        for (size_t j = 0; j < bitmapWords; j++) {
            bitmap[j].store(0, RELAXED);
        }
    }
}

template<typename IndexType>
PressureBitmap<IndexType>::PressureBitmap(PressureBitmap&& other) noexcept
    : entryCount(other.entryCount) {
    for (size_t i = 0; i < static_cast<size_t>(PoolPressure::COUNT); i++) {
        bitmaps[i] = other.bitmaps[i];
        other.bitmaps[i] = nullptr;
    }
    other.entryCount = 0;
}

template<typename IndexType>
PressureBitmap<IndexType>& PressureBitmap<IndexType>::operator=(PressureBitmap&& other) noexcept {
    if (this != &other) {
        entryCount = other.entryCount;
        for (size_t i = 0; i < static_cast<size_t>(PoolPressure::COUNT); i++) {
            bitmaps[i] = other.bitmaps[i];
            other.bitmaps[i] = nullptr;
        }
        other.entryCount = 0;
    }
    return *this;
}

template<typename IndexType>
void PressureBitmap<IndexType>::markPressure(IndexType index, PoolPressure pressure) {
    constexpr size_t BITS_PER_WORD = 64;
    const size_t bitIndex = Traits::toBitIndex(index, entryCount);
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

template<typename IndexType>
IteratorRange<typename PressureBitmap<IndexType>::BitmapIterator> PressureBitmap<IndexType>::poolsWithPressure(PoolPressure pressure) const {
    const size_t requiredBits = Traits::requiredBits(entryCount);
    Atomic<uint64_t>* bitmap = bitmaps[static_cast<size_t>(pressure)];
    return {BitmapIterator(bitmap, 0, requiredBits, entryCount), BitmapIterator(bitmap, requiredBits, requiredBits, entryCount)};
}

template<typename IndexType>
PressureBitmap<IndexType>::BitmapIterator::BitmapIterator(Atomic<uint64_t>* bitmap, size_t idx, size_t total, size_t count)
    : bitmapStart(bitmap), totalBits(total), entryCount(count), index(idx) {
#ifdef PA_BITMAP_ITERATOR_CACHE_WORD
    currentWord = 0;
#endif
    // Only advance if not at the end
    if (index < totalBits) {
        advanceToSetBit();
    }
}

template<typename IndexType>
void PressureBitmap<IndexType>::BitmapIterator::advanceToSetBit() {
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

        uint64_t word = bitmapStart[wordIndex].load(RELAXED);

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
    // Clamp to totalBits to ensure iterator equals end() when exhausted
    if (index > totalBits) {
        index = totalBits;
    }
}

template<typename IndexType>
bool PressureBitmap<IndexType>::BitmapIterator::atEnd() const {
    return index >= totalBits;
}

template<typename IndexType>
typename PressureBitmap<IndexType>::BitmapIterator& PressureBitmap<IndexType>::BitmapIterator::operator++() {
    if (index < totalBits) {
        index++;
        advanceToSetBit();
    }
    return *this;
}

template<typename IndexType>
bool PressureBitmap<IndexType>::BitmapIterator::operator==(const BitmapIterator& other) const {
    return index == other.index && bitmapStart == other.bitmapStart;
}

template<typename IndexType>
IndexType PressureBitmap<IndexType>::BitmapIterator::operator*() const {
    return Traits::fromBitIndex(index, entryCount);
}

// Explicit template instantiations
template class PressureBitmap<PoolID>;
template class PressureBitmap<size_t>;

// ==================== Range Allocator ===================

RangeAllocator::RangeAllocator(const mm::phys_memory_range allocatorRange, BootstrapAllocator bootstrapAllocator) :
context(allocatorRange, bootstrapAllocator),
globalPool(GLOBAL, context),
range(allocatorRange){
    assert(!bootstrapAllocator.isFake(), "The bootstrap allocator cannot be in measurement mode");
    assert(allocatorRange.start.value % arch::smallPageSize == 0, "Range allocator start is not page aligned");
    assert(allocatorRange.end.value % arch::smallPageSize == 0, "Range allocator end is not page aligned");

    const auto alignedTop = roundUpToNearestMultiple(allocatorRange.end.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto alignedBottom = roundDownToNearestMultiple(allocatorRange.start.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto bottomReserveCount = (allocatorRange.start.value - alignedBottom) / arch::smallPageSize;
    const auto topReserveCount = (alignedTop - allocatorRange.end.value) / arch::smallPageSize;

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

void RangeAllocator::freePages(PageRef *pages, size_t count) {
    //TODO stub
    (void)pages;
    (void)count;
}


//size_t RangeAllocator::allocatePages(size_t smallPageCount, PageAllocationCallback cb, Optional<BigPageColor> color) {

//}

// ==================== Bootstrap Code ====================
// ===================AggregateAllocator ==================

bool operator==(const RangeAllocator& a1, const RangeAllocator& a2) {
    return &a1 == &a2;
}

AggregateAllocator::AggregateAllocator(PressureBitmap<size_t> &&pressureBitmaps, Vector<RangeAllocator *> &&allocators) :
    rangePressures(move(pressureBitmaps)), allocatorList(move(allocators)) {
    for (auto allocator : allocatorList) {
        freeTree.insert(allocator);
    }
}

struct PageComparator {
    bool operator()(const PageRef& r1, const PageRef& r2) const{
        return r1.addr().value < r2.addr().value;
    }
};

bool AggregateAllocator::markPageRuns(PageRef* pages, const size_t count) {
    size_t runStart = 0;
    auto runBigPage = roundDownToNearestMultiple(pages[0].addr().value, static_cast<uint64_t>(arch::bigPageSize));
    RangeAllocator* currentRange = findAllocatorForPaddr(pages[0].addr());
    for (size_t i = 0; i < count; i++) {
        if (currentRange == nullptr)
            return false;
        const auto currBigPage = roundDownToNearestMultiple(pages[i].addr().value, static_cast<uint64_t>(arch::bigPageSize));
        if (currentRange -> range.contains(pages[i].addr())) {
            if (currBigPage == runBigPage)
                continue;
        }
        else {
            currentRange = findAllocatorForPaddr(pages[i].addr());
        }
        pages[runStart].setRunLength(i - runStart);
        runStart = i;
        runBigPage = currBigPage;
    }
    pages[runStart].setRunLength(count - runStart);
    return true;
}

RangeAllocator* AggregateAllocator::findAllocatorForPaddr(mm::phys_addr addr) {
    using Extractor = RangeAllocatorFreeTreeExtractor;

    // Helper lambda for recursive search through the tree
    auto searchTree = [&](auto& self, RangeAllocator* node) -> RangeAllocator* {
        if (node == nullptr) {
            return nullptr;
        }

        // Check if the address is in this node's range
        if (node->range.contains(addr)) {
            return node;
        }

        // Check if the address could be in the left subtree
        // Use the augmented data (subtreeRange) to prune impossible paths
        RangeAllocator* left = Extractor::left(*node);
        if (left != nullptr && left->subtreeRange.contains(addr)) {
            RangeAllocator* result = self(self, left);
            if (result != nullptr) {
                return result;
            }
        }

        // Check if the address could be in the right subtree
        RangeAllocator* right = Extractor::right(*node);
        if (right != nullptr && right->subtreeRange.contains(addr)) {
            RangeAllocator* result = self(self, right);
            if (result != nullptr) {
                return result;
            }
        }

        return nullptr;
    };

    // Start search from the root of the tree
    return searchTree(searchTree, freeTree.getRoot());
}

void AggregateAllocator::freePages(PageRef* pages, const size_t count) {
    if (count == 0) return;
    algorithm::sort(pages, count, PageComparator{});
    const auto successful = markPageRuns(pages, count);
    assert(successful, "Tried to free invalid pages");

    RangeAllocator* allocator = findAllocatorForPaddr(pages[0].addr());
    assert(allocator != nullptr, "markPageRuns succeeded but allocator not found");
    size_t runEnd = 0;
    size_t runStart = 0;

    while (runEnd < count) {
        const auto runAddr = pages[runEnd].addr();

        if (allocator->range.contains(runAddr)) {
            // Same allocator - accumulate this run
            runEnd += pages[runEnd].runLength();
        } else {
            // Different allocator - flush accumulated pages and switch
            if (runEnd > runStart) {
                allocator->freePages(&pages[runStart], runEnd - runStart);
            }

            // Switch to new allocator
            allocator = findAllocatorForPaddr(runAddr);
            assert(allocator != nullptr, "markPageRuns succeeded but allocator not found");
            runStart = runEnd;

            // Include current run
            runEnd += pages[runEnd].runLength();
        }
    }

    // Free any remaining accumulated pages
    if (runEnd > runStart) {
        assert(allocator != nullptr, "markPageRuns succeeded but allocator not found");
        allocator->freePages(&pages[runStart], runEnd - runStart);
    }
}


