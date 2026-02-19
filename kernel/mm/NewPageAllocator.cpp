//
// Created by Spencer Martin on 2/4/26.
//

#include <mem/PageAllocator.h>
#include <core/math.h>
#include <core/ds/Trees.h>

constexpr size_t bigPagesInRange(const kernel::mm::phys_memory_range range) {
    const auto alignedTop = roundUpToNearestMultiple(range.end.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, static_cast<uint64_t>(arch::bigPageSize));
    return (alignedTop - alignedBottom)/arch::bigPageSize;
}

constexpr size_t computeModerateThreshold(kernel::mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/8, MODERATE_THRESHOLD_MINIMUM);
}

constexpr size_t computeComfortableThreshold(kernel::mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/4, MODERATE_THRESHOLD_MINIMUM * 2);
}

constexpr size_t computeSurplusThreshold(kernel::mm::phys_memory_range r, size_t cpuCount) {
    auto bigPagesPerCPU = bigPagesInRange(r)/cpuCount;
    return max(bigPagesPerCPU/2, MODERATE_THRESHOLD_MINIMUM * 4);
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
    allocated += ringBuffer.bulkReadBestEffort(count - allocated, [&](auto _, SmallPageIndex index) {
        markPageFreeState(index, false);
        cb(PageRef::small(fromPageIndex(index)));
    });

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


// ==================== BigPageLinkedListExtractor ====================

// ==================== BigPageColoredLinkedListExtractor ====================

// ==================== BigPagePool ====================

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

