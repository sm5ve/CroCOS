//
// Created by Spencer Martin on 5/20/26.
//
// Bitmap-backed slab bookkeeping for fixed-capacity, single-owner /
// multi-freer allocation. Two layers:
//
//   SlabBookkeeper<N, Storage, UseHint>
//     Address-agnostic slot tracker. Holds a Core::SplitBitmap (single
//     owning allocator CPU, many freer CPUs), an atomic allocated-count
//     for hot-path occupancy queries, and an init-only reserved-count
//     for carved-out slots. Every alloc/free reports the
//     Empty/Partial/Full transition the operation caused.
//
//   Slab<N, SlotSize, Storage, UseHint>
//     Composes a SlabBookkeeper with a caller-provided backing buffer
//     and a compile-time slot size. Returns void* on alloc, takes void*
//     on free. The buffer is caller-owned.
//
// SlotCount must be a positive multiple of 64 (the underlying SplitBitmap
// works in whole 64-bit words). Storage defaults to inline; pass
// Core::ExternalSplitBitmapStorage to use caller-managed bitmap memory.
//

#ifndef CROCOS_LIBALLOC_SLAB_H
#define CROCOS_LIBALLOC_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <core/atomic.h>
#include <core/atomic/SplitBitmap.h>
#include <core/TypeTraits.h>
#include <core/math.h>
#include <core/utility.h>
#include <liballoc/OccupancyTransition.h>

namespace LibAlloc {

template <size_t SlotCount,
          typename Storage = Core::InlineSplitBitmapStorage<SlotCount / 64>,
          bool UseHint = ((SlotCount / 64) > 1)>
class SlabBookkeeper {
    static_assert(SlotCount > 0 && (SlotCount % 64) == 0,
                  "SlabBookkeeper SlotCount must be a positive multiple of 64");

    static constexpr size_t kWordCount = SlotCount / 64;
    using Bitmap = Core::SplitBitmap<kWordCount, Storage, UseHint>;

public:
    using SlotIndexType = SmallestUInt_t<log2ceil(SlotCount)>;
    using SlotCountType = SmallestUInt_t<log2ceil(SlotCount + 1)>;

    static constexpr size_t kSlotCount = SlotCount;
    static constexpr size_t kBitmapWordCount = kWordCount;

    // Default ctor only sensible for inline storage. Both halves of the
    // bitmap are left in their underlying SplitBitmap-default state
    // (freeBitmap zero-initialized, allocBitmap uninitialized) — the
    // caller must call seedAllAvailable() or seedAllUsed() before use.
    SlabBookkeeper() = default;

    // External-storage constructor. Only valid when Storage is
    // Core::ExternalSplitBitmapStorage<kWordCount> (compile error otherwise).
    constexpr SlabBookkeeper(uint64_t* allocPtr, Atomic<uint64_t>* freePtr)
        : bitmap(allocPtr, freePtr) {}

    void seedAllAvailable() {
        bitmap.seedAllAvailable();
        allocatedCount.store(0, RELEASE);
    }

    void seedAllUsed() {
        bitmap.seedAllUsed();
        allocatedCount.store(static_cast<SlotCountType>(SlotCount - reservedCount), RELEASE);
    }

    // Owner-only single-slot claim. Returns the claimed slot index, or
    // -1 if the slab is exhausted. Always updates `transition`.
    [[nodiscard]] int allocSlot(Core::OccupancyTransition& transition) {
        const size_t maxAlloc = SlotCount - reservedCount;
        int bit = bitmap.tryClaimBitNoDrain();
        if (bit < 0) {
            if (bitmap.drainFreeIntoAlloc()) {
                bit = bitmap.tryClaimBitNoDrain();
            }
        }
        if (bit < 0) {
            const size_t cur = static_cast<size_t>(allocatedCount.load(ACQUIRE));
            transition.before = stateFromCount(cur, maxAlloc);
            transition.after  = transition.before;
            return -1;
        }
        const size_t prev = static_cast<size_t>(allocatedCount.fetch_add(1, ACQ_REL));
        transition.before = stateFromCount(prev, maxAlloc);
        transition.after  = stateFromCount(prev + 1, maxAlloc);
        return bit;
    }

    // Owner-only multi-slot claim. Invokes `cb` once per claimed slot,
    // with the slot index. Returns the count actually claimed (may be
    // less than `count` if the slab is exhausted). Always updates
    // `transition` to span the whole batch.
    size_t allocSlots(size_t count,
                      FunctionRef<void(size_t)> cb,
                      Core::OccupancyTransition& transition) {
        size_t allocated = 0;
        const size_t maxAlloc = SlotCount - reservedCount;
        while (allocated < count) {
            int bit = bitmap.tryClaimBitNoDrain();
            if (bit < 0) {
                if (!bitmap.drainFreeIntoAlloc()) break;
                bit = bitmap.tryClaimBitNoDrain();
                if (bit < 0) break;
            }
            cb(static_cast<size_t>(bit));
            allocated++;
        }
        const size_t prev = static_cast<size_t>(allocatedCount.fetch_add(
                static_cast<SlotCountType>(allocated), ACQ_REL));
        transition.before = stateFromCount(prev, maxAlloc);
        transition.after  = stateFromCount(prev + allocated, maxAlloc);
        return allocated;
    }

    // Multi-CPU safe single-slot release.
    void freeSlot(size_t slot, Core::OccupancyTransition& transition) {
        const size_t maxAlloc = SlotCount - reservedCount;
        bitmap.releaseBit(slot);
        const size_t prev = static_cast<size_t>(allocatedCount.fetch_sub(1, ACQ_REL));
        transition.before = stateFromCount(prev, maxAlloc);
        transition.after  = stateFromCount(prev - 1, maxAlloc);
    }

    // Multi-CPU safe bulk release. `pending` is a per-word bitmask of
    // slots to release; `releasedCount` is the total number of bits set
    // across all words. The caller is responsible for coalescing — this
    // mirrors Core::SplitBitmap::releaseBitsBulk's contract.
    void freeSlotsBulk(const uint64_t pending[kWordCount],
                       size_t releasedCount,
                       Core::OccupancyTransition& transition) {
        const size_t maxAlloc = SlotCount - reservedCount;
        bitmap.releaseBitsBulk(pending);
        const size_t prev = static_cast<size_t>(allocatedCount.fetch_sub(
                static_cast<SlotCountType>(releasedCount), ACQ_REL));
        transition.before = stateFromCount(prev, maxAlloc);
        transition.after  = stateFromCount(prev - releasedCount, maxAlloc);
    }

    // Init-only: remove a slot from circulation entirely. No-op if the
    // slot is not currently available. Asserts the slab is quiescent.
    void reserveSlot(size_t slot) {
        assert(allocatedCount.load(RELAXED) == 0,
               "SlabBookkeeper::reserveSlot only valid before any alloc");
        if (bitmap.isBitAvailable(slot)) {
            bitmap.reserveBit(slot);
            reservedCount++;
        }
    }

    // Hand allocBitmap bits back to freeBitmap. Called when the owning
    // allocator CPU yields the slab to another CPU.
    void flushAllocSide() { bitmap.flushAllocIntoFree(); }

    [[nodiscard]] bool isSlotFree(size_t slot) const { return bitmap.isBitAvailable(slot); }

    [[nodiscard]] bool isFull() const {
        return allocatedCount.load(ACQUIRE)
               == static_cast<SlotCountType>(SlotCount - reservedCount);
    }

    [[nodiscard]] bool isEmpty() const {
        return allocatedCount.load(ACQUIRE) == 0 && reservedCount == 0;
    }

    [[nodiscard]] bool hasReservedSlots() const { return reservedCount > 0; }

    [[nodiscard]] size_t allocatedSlotCount() const {
        return static_cast<size_t>(allocatedCount.load(ACQUIRE));
    }

    [[nodiscard]] size_t reservedSlotCount() const {
        return static_cast<size_t>(reservedCount);
    }

    [[nodiscard]] size_t freeSlotCount() const {
        return (SlotCount - static_cast<size_t>(reservedCount))
               - static_cast<size_t>(allocatedCount.load(ACQUIRE));
    }

    // Snapshot popcount across both halves of the bitmap. Exact only when
    // the slab is quiescent.
    [[nodiscard]] size_t bitmapAvailableCount() const { return bitmap.countAvailable(); }

    static Core::OccupancyState stateFromCount(size_t count, size_t maxAlloc) {
        if (count == 0) return Core::OccupancyState::Empty;
        if (count >= maxAlloc) return Core::OccupancyState::Full;
        return Core::OccupancyState::Partial;
    }

private:
    Bitmap bitmap;
    Atomic<SlotCountType> allocatedCount{0};
    SlotCountType reservedCount = 0;
};

template <size_t SlotCount,
          size_t SlotSize,
          typename Storage = Core::InlineSplitBitmapStorage<SlotCount / 64>,
          bool UseHint = ((SlotCount / 64) > 1)>
class Slab {
public:
    using Bookkeeper = SlabBookkeeper<SlotCount, Storage, UseHint>;
    static constexpr size_t kSlotCount = SlotCount;
    static constexpr size_t kSlotSize  = SlotSize;
    static constexpr size_t kRegionSize = SlotCount * SlotSize;

    // Inline-storage constructor: caller owns the backing buffer; bitmap
    // storage is inline. Seeds the bookkeeper so the slab is immediately
    // usable.
    explicit Slab(void* base) : baseAddr(static_cast<uint8_t*>(base)) {
        bookkeeper.seedAllAvailable();
    }

    // External-storage constructor: caller provides both the backing
    // buffer and the bitmap word pointers. Only valid when Storage is
    // Core::ExternalSplitBitmapStorage.
    Slab(void* base, uint64_t* allocPtr, Atomic<uint64_t>* freePtr)
        : bookkeeper(allocPtr, freePtr), baseAddr(static_cast<uint8_t*>(base)) {
        bookkeeper.seedAllAvailable();
    }

    [[nodiscard]] void* alloc(Core::OccupancyTransition& transition) {
        const int slot = bookkeeper.allocSlot(transition);
        if (slot < 0) return nullptr;
        return baseAddr + static_cast<size_t>(slot) * SlotSize;
    }
    [[nodiscard]] void* alloc() { Core::OccupancyTransition t{}; return alloc(t); }

    void free(void* p, Core::OccupancyTransition& transition) {
        assert(contains(p), "Slab::free called on out-of-range pointer");
        const size_t off = static_cast<size_t>(static_cast<uint8_t*>(p) - baseAddr);
        assert(off % SlotSize == 0, "Slab::free called on misaligned pointer");
        bookkeeper.freeSlot(off / SlotSize, transition);
    }
    void free(void* p) { Core::OccupancyTransition t{}; free(p, t); }

    [[nodiscard]] bool contains(const void* p) const {
        const auto* q = static_cast<const uint8_t*>(p);
        return q >= baseAddr && q < baseAddr + kRegionSize;
    }

    [[nodiscard]] bool isFull()  const { return bookkeeper.isFull(); }
    [[nodiscard]] bool isEmpty() const { return bookkeeper.isEmpty(); }
    [[nodiscard]] size_t freeSlotCount()      const { return bookkeeper.freeSlotCount(); }
    [[nodiscard]] size_t allocatedSlotCount() const { return bookkeeper.allocatedSlotCount(); }

    [[nodiscard]] void* baseAddress() const { return baseAddr; }

    Bookkeeper&       bookkeeping()       { return bookkeeper; }
    const Bookkeeper& bookkeeping() const { return bookkeeper; }

private:
    Bookkeeper bookkeeper;
    uint8_t*   baseAddr;
};

} // namespace LibAlloc

#endif // CROCOS_LIBALLOC_SLAB_H
