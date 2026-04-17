//
// Created by Spencer Martin on 4/7/26.
//

#ifndef CROCOS_ATOMICBITPOOL_H
#define CROCOS_ATOMICBITPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <core/atomic.h>

class AtomicBitPool {
    struct Entry {
        Atomic<uint64_t> bitmap;
        Atomic<int64_t> count;
    };

    // Result returned by incrementCount / decrementCount.
    // toggledTopLevel is true when a bitmap XOR was fired on the LN (top-level)
    // bitmap from the level below it (levelCount - 2).
    // poolTransitioned meaning:
    //   - for incrementCount: the LN bitmap was zero before the XOR (AddedToEmpty signal)
    //   - for decrementCount: the LN bitmap became zero after the XOR (RemovedAndMadeEmpty signal)
    struct CountResult {
        int64_t  newCount;
        bool     toggledTopLevel;
        bool     poolTransitioned;
    };

    void* storage;
    size_t entryStride;
    size_t levelCount;
    size_t l1BitmapLog2Width;
    size_t capacity;

    [[nodiscard]] size_t localIndex(size_t absoluteIndex, size_t level) const;
    [[nodiscard]] Entry& entryAt(size_t index, size_t level);
    [[nodiscard]] const Entry& entryAt(size_t index, size_t level) const;

    [[nodiscard]] size_t shiftForLevel(size_t level) const;
    [[nodiscard]] size_t cumulativeShift(size_t level) const;
    [[nodiscard]] size_t bitIndexForLevel(size_t absoluteIndex, size_t level) const;
    [[nodiscard]] size_t entryIndexForLevel(size_t absoluteIndex, size_t level) const;
    [[nodiscard]] size_t childEntryIndex(size_t entryIndex, size_t bitIndex, size_t level) const;

    [[nodiscard]] size_t parentEntryIndex(size_t entryIndex, size_t level) const;
    [[nodiscard]] size_t bitIndexInParent(size_t entryIndex, size_t level) const;

    CountResult incrementCount(size_t entryIndex, size_t level);
    CountResult decrementCount(size_t entryIndex, size_t level);

public:
    enum class AddResult {
        AlreadyPresent,
        AddedToNonempty,
        AddedToEmpty,
    };

    enum class RemoveResult {
        NotPresent,
        RemovedAndMadeEmpty,
        RemovedAndStayedNonempty,
    };

    enum class GetResult {
        Success,
        Empty,
        Contended,
    };

    static constexpr size_t maxLevelCount = 10;
    static size_t requiredBufferSize(size_t capacity, size_t entryStride = 64);

    AtomicBitPool(size_t capacity, void* storage, size_t entryStride = 64);
    AtomicBitPool(AtomicBitPool&&) noexcept;

    AddResult add(size_t absoluteIndex);
    RemoveResult remove(size_t absoluteIndex);

    GetResult getAny(size_t threadId, size_t &outIndex, size_t maxRetries = 16);
#ifdef CROCOS_TESTING
    [[nodiscard]] bool checkInvariants() const;

    // Returns true if the given index is currently set in the pool.
    // Only safe to call in quiescent (single-threaded) state.
    [[nodiscard]] bool isSet(size_t absoluteIndex) const {
        const size_t bitIdx   = bitIndexForLevel(absoluteIndex, 0);
        const size_t entryIdx = entryIndexForLevel(absoluteIndex, 0);
        return (entryAt(entryIdx, 0).bitmap.load(RELAXED) >> bitIdx) & 1u;
    }

    // Count of all currently set indices.  Only safe in quiescent state.
    [[nodiscard]] size_t countSet() const;
#endif
};

#endif //CROCOS_ATOMICBITPOOL_H