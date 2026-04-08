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

    void* storage;
    size_t entryStride;
    size_t levelCount;
    size_t l1BitmapLog2Width;

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

    int64_t incrementCount(size_t entryIndex, size_t level);
    int64_t decrementCount(size_t entryIndex, size_t level);

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

    AddResult add(size_t absoluteIndex);
    RemoveResult remove(size_t absoluteIndex);

    GetResult getAny(size_t threadId, size_t &outIndex, size_t maxRetries = 16);
#ifdef CROCOS_TESTING
    [[nodiscard]] bool checkInvariants() const;
#endif
};

#endif //CROCOS_ATOMICBITPOOL_H