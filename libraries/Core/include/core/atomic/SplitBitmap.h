//
// Created by Spencer Martin on 5/20/26.
//
// SplitBitmap: a fixed-width "alloc + free" bitmap pair shared between
// a single allocator thread and many freer threads.
//
// Bit semantics:
//   bit set in allocBitmap → available for the single owning allocator CPU
//   bit set in freeBitmap  → just-freed; not yet drained back to the alloc side
//   bit set in neither     → currently allocated
//
// The single allocator CPU writes allocBitmap non-atomically. Any CPU may
// release a bit via the atomic freeBitmap. When allocBitmap runs dry, the
// allocator drains the freeBitmap with an atomic exchange and OR-merges it
// back into allocBitmap.
//
// Storage policies:
//   InlineSplitBitmapStorage<W>   owns its two arrays inline (cache-aligned).
//   ExternalSplitBitmapStorage<W> takes pointers to caller-managed storage,
//                                  for sites where the alloc and free arrays
//                                  must live at distinct offsets in a buffer.
//

#ifndef CROCOS_SPLITBITMAP_H
#define CROCOS_SPLITBITMAP_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <core/atomic.h>
#include <core/utility.h>

namespace Core {

template <size_t WordCount>
struct InlineSplitBitmapStorage {
    alignas(64) uint64_t allocBitmap[WordCount];
    alignas(64) Atomic<uint64_t> freeBitmap[WordCount]{};

    [[nodiscard]] uint64_t*               allocWords()       { return allocBitmap; }
    [[nodiscard]] Atomic<uint64_t>*       freeWords()        { return freeBitmap;  }
    [[nodiscard]] const uint64_t*         allocWords() const { return allocBitmap; }
    [[nodiscard]] const Atomic<uint64_t>* freeWords()  const { return freeBitmap;  }
};

template <size_t WordCount>
struct ExternalSplitBitmapStorage {
    uint64_t*         allocPtr;
    Atomic<uint64_t>* freePtr;

    constexpr ExternalSplitBitmapStorage(uint64_t* a, Atomic<uint64_t>* f)
        : allocPtr(a), freePtr(f) {}

    [[nodiscard]] uint64_t*               allocWords()       { return allocPtr; }
    [[nodiscard]] Atomic<uint64_t>*       freeWords()        { return freePtr;  }
    [[nodiscard]] const uint64_t*         allocWords() const { return allocPtr; }
    [[nodiscard]] const Atomic<uint64_t>* freeWords()  const { return freePtr;  }
};

struct EmptyHint {};

template <size_t WordCount,
          typename Storage = InlineSplitBitmapStorage<WordCount>,
          bool UseHint = (WordCount > 1)>
class SplitBitmap {
    [[no_unique_address]] Storage storage;
    [[no_unique_address]] conditional_t<UseHint, uint8_t, EmptyHint> hint{};

    [[nodiscard]] constexpr size_t firstScanWord() const {
        if constexpr (UseHint) return static_cast<size_t>(hint);
        else                   return 0;
    }

    constexpr void setHint(size_t w) {
        if constexpr (UseHint) hint = static_cast<uint8_t>(w);
    }

public:
    static constexpr size_t kWordCount = WordCount;
    static constexpr size_t kBitCount  = WordCount * 64;

    // Default ctor only sensible for Inline storage; freeBitmap atomics are
    // value-initialized to zero by the storage policy, allocBitmap is
    // uninitialized (caller must seedAllAvailable / seedAllUsed before use).
    SplitBitmap() = default;

    // External storage requires the caller to provide pointers up front.
    constexpr SplitBitmap(uint64_t* allocPtr, Atomic<uint64_t>* freePtr)
        : storage(allocPtr, freePtr) {}

    // Sets all bits to "available" (alloc=~0, free=0). Resets hint.
    void seedAllAvailable() {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        for (size_t w = 0; w < WordCount; w++) {
            a[w] = ~uint64_t{0};
            f[w].store(0, RELEASE);
        }
        setHint(0);
    }

    // Sets all bits to "in use" (both halves 0). Resets hint.
    void seedAllUsed() {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        for (size_t w = 0; w < WordCount; w++) {
            a[w] = 0;
            f[w].store(0, RELEASE);
        }
        setHint(0);
    }

    // Claim a bit by scanning allocBitmap only — no drain.
    // Returns the bit index claimed, or -1 if the alloc side is empty.
    [[nodiscard]] int tryClaimBitNoDrain() {
        uint64_t* a = storage.allocWords();
        for (size_t w = firstScanWord(); w < WordCount; w++) {
            if (a[w]) {
                const int bit = __builtin_ctzll(a[w]);
                a[w] &= a[w] - 1;
                if constexpr (UseHint) {
                    if (!a[w] && w == static_cast<size_t>(hint)) {
                        hint = static_cast<uint8_t>(w + 1);
                    }
                }
                return static_cast<int>(w * 64u + static_cast<unsigned>(bit));
            }
            if constexpr (UseHint) {
                if (w == static_cast<size_t>(hint)) {
                    hint = static_cast<uint8_t>(w + 1);
                }
            }
        }
        return -1;
    }

    // Drain freeBitmap into allocBitmap with per-word atomic exchange.
    // Returns true if any bits were drained.
    bool drainFreeIntoAlloc() {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        size_t newHint = WordCount;
        bool drained = false;
        for (size_t w = 0; w < WordCount; w++) {
            const uint64_t freed = f[w].exchange(0ull, ACQ_REL);
            if (freed) {
                a[w] |= freed;
                drained = true;
                if (newHint == WordCount) newHint = w;
            }
        }
        if constexpr (UseHint) {
            if (drained) hint = static_cast<uint8_t>(newHint);
        }
        return drained;
    }

    // tryClaimBitNoDrain, then on miss drainFreeIntoAlloc + retry.
    // Returns -1 only if both halves are empty.
    [[nodiscard]] int claimBit() {
        int bit = tryClaimBitNoDrain();
        if (bit >= 0) return bit;
        if (!drainFreeIntoAlloc()) return -1;
        return tryClaimBitNoDrain();
    }

    struct ReleaseResult {
        // True iff the freeBitmap word for this bit was zero before this
        // release set the bit. Used by VMSubstrate's radix tree to decide
        // when to bump leafFreeWordCount and propagate availability upward.
        bool wordWasZero;
    };

    // Multi-CPU safe.
    ReleaseResult releaseBit(size_t bit) {
        Atomic<uint64_t>* f = storage.freeWords();
        const size_t w = bit / 64;
        const uint64_t mask = uint64_t{1} << (bit % 64);
        const uint64_t prev = f[w].fetch_or(mask, RELEASE);
        assert(!(prev & mask), "Double free: bit already set in freeBitmap");
        return { prev == 0 };
    }

    // Batch release for callers that have already coalesced freed bits into
    // a per-word mask. Performs the same double-free assertions per word.
    void releaseBitsBulk(const uint64_t pending[WordCount]) {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        for (size_t w = 0; w < WordCount; w++) {
            if (!pending[w]) continue;
            // Whichever CPU owns the alloc side: a freed bit must not already
            // appear there (that would mean the slot was never allocated).
            assert(!(atomic_load(a[w]) & pending[w]),
                   "Double free: bit set in allocBitmap during release");
            const uint64_t prev = f[w].fetch_or(pending[w], RELEASE);
            assert(!(prev & pending[w]),
                   "Double free: bit already set in freeBitmap");
        }
    }

    // Single-CPU side: take a bit out of circulation entirely.
    // Used at init time for reservations (e.g. memory map carve-outs).
    void reserveBit(size_t bit) {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        const size_t w = bit / 64;
        const uint64_t mask = uint64_t{1} << (bit % 64);
        a[w] &= ~mask;
        f[w].fetch_and(~mask, RELEASE);
        // Leave hint alone: pointing one word too low only costs one extra
        // word scan on the next claim.
    }

    // Hand alloc-side bits back to the free side. Called when the allocator
    // CPU is yielding ownership of this bitmap to another CPU.
    void flushAllocIntoFree() {
        uint64_t* a = storage.allocWords();
        Atomic<uint64_t>* f = storage.freeWords();
        for (size_t w = 0; w < WordCount; w++) {
            if (a[w]) {
                f[w].fetch_or(a[w], RELEASE);
                a[w] = 0;
            }
        }
        setHint(0);
    }

    [[nodiscard]] bool isBitAvailable(size_t bit) const {
        const uint64_t* a = storage.allocWords();
        const Atomic<uint64_t>* f = storage.freeWords();
        const size_t w = bit / 64;
        const uint64_t mask = uint64_t{1} << (bit % 64);
        return (a[w] & mask) || (f[w].load(ACQUIRE) & mask);
    }

    // Exact only when the bitmap is quiescent; a snapshot under concurrent
    // releases.
    [[nodiscard]] size_t countAvailable() const {
        const uint64_t* a = storage.allocWords();
        const Atomic<uint64_t>* f = storage.freeWords();
        size_t total = 0;
        for (size_t w = 0; w < WordCount; w++) {
            total += static_cast<size_t>(__builtin_popcountll(a[w]));
            total += static_cast<size_t>(__builtin_popcountll(f[w].load(RELAXED)));
        }
        return total;
    }

    [[nodiscard]] bool allocSideEmpty() const {
        const uint64_t* a = storage.allocWords();
        for (size_t w = 0; w < WordCount; w++) {
            if (a[w]) return false;
        }
        return true;
    }
};

} // namespace Core

#endif //CROCOS_SPLITBITMAP_H
