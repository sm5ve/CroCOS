//
// Created by Spencer Martin on 4/7/26.
//
#include <core/atomic/AtomicBitPool.h>
#include <assert.h>
#include <core/math.h>

static size_t computeLevelCount(const size_t capacity) {
    if (capacity <= 64) return 1;
    // smallest n s.t. 64^n >= capacity
    // 64^n = 2^(6n), so we need 6n >= log2ceil(capacity)
    return divideAndRoundUp(log2ceil(capacity), static_cast<size_t>(6));
}

static size_t computeL1BitmapLog2Width(size_t capacity, size_t levelCount) {
    const size_t out = log2ceil(capacity);
    const size_t shift = 6 * (levelCount - 1);
    return out - shift;
}

size_t AtomicBitPool::requiredBufferSize(size_t capacity, size_t entryStride) {
    capacity = (1ull << log2ceil(capacity));
    entryStride = max(entryStride, sizeof(Entry));
    size_t levelCount = computeLevelCount(capacity);
    size_t total = 0;
    // L0
    total += entryStride * divideAndRoundUp(capacity, size_t(64));
    // L1 through LN
    size_t levelEntries = 1;
    //levelCount is always at least 1
    for (size_t level = levelCount - 1; level >= 1; level--) {
        total += entryStride * levelEntries;
        levelEntries *= 64;
    }
    return total;
}

AtomicBitPool::AtomicBitPool(size_t capacity, void* buffer, size_t stride) {
    entryStride = max(stride, sizeof(Entry));
    capacity = (1ull << log2ceil(capacity));
    this->storage = buffer;
    this->entryStride = stride;
    this->levelCount = computeLevelCount(capacity);
    this->l1BitmapLog2Width = computeL1BitmapLog2Width(capacity, levelCount);
    memset(storage, 0, requiredBufferSize(capacity, stride));
}

static constexpr size_t geometricSum = ((1ull << (6 * AtomicBitPool::maxLevelCount)) - 1) / 63;

AtomicBitPool::Entry& AtomicBitPool::entryAt(size_t index, size_t level) {
    size_t shift = 6 * (maxLevelCount + level - levelCount + 1);
    size_t offset = entryStride * (geometricSum >> shift);
    auto* base = reinterpret_cast<uint8_t*>(storage) + offset;
    return *reinterpret_cast<Entry*>(base + index * entryStride);
}

const AtomicBitPool::Entry& AtomicBitPool::entryAt(size_t index, size_t level) const{
    size_t shift = 6 * (maxLevelCount + level - levelCount + 1);
    size_t offset = entryStride * (geometricSum >> shift);
    auto* base = reinterpret_cast<uint8_t*>(storage) + offset;
    return *reinterpret_cast<Entry*>(base + index * entryStride);
}

size_t AtomicBitPool::shiftForLevel(size_t level) const {
    return (level == 1) ? l1BitmapLog2Width : 6;
}

size_t AtomicBitPool::cumulativeShift(size_t level) const {
    size_t shift = 6 * level;
    if (level >= 2) shift -= (6 - l1BitmapLog2Width);
    return shift;
}

size_t AtomicBitPool::bitIndexForLevel(size_t absoluteIndex, size_t level) const {
    return (absoluteIndex >> cumulativeShift(level)) & ((1ull << shiftForLevel(level)) - 1);
}

size_t AtomicBitPool::entryIndexForLevel(size_t absoluteIndex, size_t level) const {
    return absoluteIndex >> (cumulativeShift(level) + shiftForLevel(level));
}

size_t AtomicBitPool::childEntryIndex(size_t entryIndex, size_t bitIndex, size_t level) const {
    return entryIndex * (1ull << shiftForLevel(level)) + bitIndex;
}

size_t AtomicBitPool::parentEntryIndex(size_t entryIndex, size_t level) const {
    return entryIndex >> shiftForLevel(level + 1);
}

size_t AtomicBitPool::bitIndexInParent(size_t entryIndex, size_t level) const {
    size_t parentShift = shiftForLevel(level + 1);
    return entryIndex & ((1ull << parentShift) - 1);
}

int64_t AtomicBitPool::incrementCount(size_t entryIndex, size_t level) {
    int64_t oldCount = entryAt(entryIndex, level).count.fetch_add(1);
    if (oldCount == 0 && level < levelCount - 1)
        entryAt(parentEntryIndex(entryIndex, level), level + 1)
            .bitmap.fetch_xor(1ull << bitIndexInParent(entryIndex, level));
    return oldCount + 1;
}

int64_t AtomicBitPool::decrementCount(size_t entryIndex, size_t level) {
    int64_t oldCount = entryAt(entryIndex, level).count.fetch_sub(1);
    if (oldCount == 1 && level < levelCount - 1)
        entryAt(parentEntryIndex(entryIndex, level), level + 1)
            .bitmap.fetch_xor(1ull << bitIndexInParent(entryIndex, level));
    return oldCount - 1;
}

AtomicBitPool::AddResult AtomicBitPool::add(size_t absoluteIndex) {
    size_t l0Entry = entryIndexForLevel(absoluteIndex, 0);
    size_t l0Bit = bitIndexForLevel(absoluteIndex, 0);
    uint64_t mask = uint64_t(1) << l0Bit;
    uint64_t old = entryAt(l0Entry, 0).bitmap.fetch_or(mask);
    if (old & mask) return AddResult::AlreadyPresent;

    for (size_t k = 0; k < levelCount; k++) {
        size_t entry = entryIndexForLevel(absoluteIndex, k);
        bool wasEmpty = (incrementCount(entry, k) == 1);
        if (k == levelCount - 1)
            return wasEmpty ? AddResult::AddedToEmpty : AddResult::AddedToNonempty;
    }

    // unreachable
    __builtin_unreachable();
}

AtomicBitPool::RemoveResult AtomicBitPool::remove(size_t absoluteIndex) {
    // Phase 1: walk downward decrementing counts, capturing top level transition
    bool topWasOne = false;
    for (size_t k = levelCount; k-- > 0;) {
        size_t entry = entryIndexForLevel(absoluteIndex, k);
        int64_t newCount = decrementCount(entry, k);
        if (newCount < 0) {
            // back out: undo the decrement at level k, then restore all higher
            // levels that were already decremented in earlier loop iterations
            for (size_t j = k; j < levelCount; j++)
                incrementCount(entryIndexForLevel(absoluteIndex, j), j);
            return RemoveResult::NotPresent;
        }
        if (k == levelCount - 1)
            topWasOne = (newCount == 0);
    }

    // Phase 2: attempt to clear the L0 bit
    size_t l0Entry = entryIndexForLevel(absoluteIndex, 0);
    size_t l0Bit = bitIndexForLevel(absoluteIndex, 0);
    uint64_t mask = uint64_t(1) << l0Bit;
    uint64_t old = entryAt(l0Entry, 0).bitmap.fetch_and(~mask);
    if (!(old & mask)) {
        // bit was already clear, back out all count decrements
        for (size_t j = 0; j < levelCount; j++)
            incrementCount(entryIndexForLevel(absoluteIndex, j), j);
        return RemoveResult::NotPresent;
    }

    return topWasOne ? RemoveResult::RemovedAndMadeEmpty : RemoveResult::RemovedAndStayedNonempty;
}

AtomicBitPool::GetResult AtomicBitPool::getAny(size_t threadId, size_t& outIndex, size_t maxRetries) {
    size_t retryCount = 0;
    size_t level = levelCount - 1;
    size_t entryIndex = 0;

    size_t decrementedAt[maxLevelCount];
    for (size_t i = 0; i < levelCount; i++) decrementedAt[i] = SIZE_MAX;

    auto doRotation = [&](uint64_t bitmap) -> size_t {
        size_t hash = threadId ^ (retryCount * 2654435761ULL);
        size_t r = hash & 63;
        bool useLzcnt = hash & 64;
        uint64_t rotated = (bitmap << r) | (bitmap >> (64 - r));
        size_t b = useLzcnt ? (63 - countLeadingZeros(rotated))
                            : countTrailingZeros(rotated);
        return (b + 64 - r) & 63;
    };

    auto backout = [&]() {
        for (size_t k = 0; k < levelCount; k++) {
            if (decrementedAt[k] != SIZE_MAX) {
                incrementCount(decrementedAt[k], k);
                decrementedAt[k] = SIZE_MAX;
            }
        }
    };

    while (retryCount <= maxRetries) {
        if (level == 0) {
            uint64_t bitmap = entryAt(entryIndex, 0).bitmap.load();
            // For levelCount == 1 the root is the L0 entry itself; the higher-level
            // empty check is never reached, so we must detect an empty bitmap here.
            if (bitmap == 0 && levelCount == 1) {
                backout();
                return GetResult::Empty;
            }
            size_t actualBit = doRotation(bitmap);
            uint64_t mask = uint64_t(1) << actualBit;
            uint64_t old = entryAt(entryIndex, 0).bitmap.fetch_and(~mask);
            if (old & mask) {
                // Successfully claimed the bit.  The downward traversal decremented
                // counts for levels 0..levelCount-2 (via the higher-level loop), but
                // the root count at level levelCount-1 (always entry 0) was never
                // touched.  Decrement it now to keep all counts consistent.
                decrementCount(0, levelCount - 1);
                outIndex = (entryIndex << 6) | actualBit;
                return GetResult::Success;
            }
            // raced, retry at L0
            retryCount++;
            continue;
        }

        // Higher level: use hint bitmap to select a child
        uint64_t bitmap = entryAt(entryIndex, level).bitmap.load();
        if (bitmap == 0) {
            if (level == levelCount - 1) {
                backout();
                return GetResult::Empty;
            }
            // The decrement that brought us to this level is no longer valid
            // (the subtree turned empty concurrently). Undo it before backing up,
            // otherwise the entry's count leaks -1 and corrupts the tree invariant.
            if (decrementedAt[level] != SIZE_MAX) {
                incrementCount(decrementedAt[level], level);
                decrementedAt[level] = SIZE_MAX;
            }
            // back up to parent
            level++;
            entryIndex = parentEntryIndex(entryIndex, level - 1);
            retryCount++;
            continue;
        }
        size_t actualBit = doRotation(bitmap);
        size_t childEntry = childEntryIndex(entryIndex, actualBit, level);

        int64_t newCount = decrementCount(childEntry, level - 1);
        if (newCount < 0) {
            incrementCount(childEntry, level - 1);
            retryCount++;
            continue;
        }
        decrementedAt[level - 1] = childEntry;
        level--;
        entryIndex = childEntry;
    }

    backout();
    return GetResult::Contended;
}

#ifdef CROCOS_TESTING
bool AtomicBitPool::checkInvariants() const {
    size_t numEntries = 1;
    for (size_t k = levelCount - 1; k-- > 0;) {
        size_t fanout = size_t(1) << shiftForLevel(k + 1);
        size_t numChildEntries = numEntries * fanout;
        for (size_t e = 0; e < numEntries; e++) {
            int64_t expectedCount = 0;
            uint64_t expectedBitmap = 0;
            for (size_t c = 0; c < fanout; c++) {
                size_t childEntry = e * fanout + c;
                int64_t childCount = entryAt(childEntry, k).count.load();
                expectedCount += childCount;
                if (childCount > 0)
                    expectedBitmap |= uint64_t(1) << c;
            }
            const Entry& entry = entryAt(e, k + 1);
            if (entry.count.load() != expectedCount) return false;
            if (entry.bitmap.load() != expectedBitmap) return false;
        }
        numEntries = numChildEntries;
    }
    // Check L0: count should equal popcount of bitmap
    for (size_t e = 0; e < numEntries; e++) {
        const Entry& entry = entryAt(e, 0);
        uint64_t bitmap = entry.bitmap.load();
        if (entry.count.load() != __builtin_popcountll(bitmap)) return false;
    }
    return true;
}
#endif