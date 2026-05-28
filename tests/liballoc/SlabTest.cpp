//
// Unit tests for LibAlloc::SlabBookkeeper and LibAlloc::Slab
// Created by Spencer Martin on 5/20/26.
//
// Coverage:
//   - seedAllAvailable / seedAllUsed
//   - allocSlot / allocSlots return unique slot indices
//   - OccupancyTransition: Empty -> Partial -> Full edges on alloc, and
//     the inverse on free
//   - freeSlot / freeSlotsBulk round-trip
//   - reserveSlot removes a slot from circulation and adjusts isFull/empty
//   - External-storage construction
//   - Full-fat Slab address ↔ index translation, contains() bounds check
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <liballoc/Slab.h>

#include <set>
#include <cstdint>

using namespace CroCOSTest;
using LibAlloc::SlabBookkeeper;
using LibAlloc::Slab;
using Core::OccupancyState;
using Core::OccupancyTransition;
using Core::InlineSplitBitmapStorage;
using Core::ExternalSplitBitmapStorage;

// Ceiling-division word count: SlabBookkeeper<N>::kBitmapWordCount equals
// (N + 63) / 64. SplitBitmap storage is cache-line-aligned, so raw sizeof
// only grows once the bitmaps exceed a cache line — we check the logical
// invariant (kBitmapWordCount) plus the sizeof identity within a single
// equivalence class.
static_assert(SlabBookkeeper<1>::kBitmapWordCount == 1);
static_assert(SlabBookkeeper<63>::kBitmapWordCount == 1);
static_assert(SlabBookkeeper<64>::kBitmapWordCount == 1);
static_assert(SlabBookkeeper<65>::kBitmapWordCount == 2);
static_assert(SlabBookkeeper<128>::kBitmapWordCount == 2);
static_assert(SlabBookkeeper<129>::kBitmapWordCount == 3);
static_assert(sizeof(SlabBookkeeper<63>) == sizeof(SlabBookkeeper<64>));
static_assert(sizeof(SlabBookkeeper<127>) == sizeof(SlabBookkeeper<128>));
// kTailBits = kBitmapWordCount * 64 - SlotCount.
static_assert(SlabBookkeeper<64>::kTailBits == 0);
static_assert(SlabBookkeeper<65>::kTailBits == 63);
static_assert(SlabBookkeeper<137>::kTailBits == (3 * 64 - 137));

// ============================================================
// SlabBookkeeper — basic alloc/free, single-slot path
// ============================================================

TEST(SlabBookkeeper_SeedAvailable_AllocAllSlots) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    ASSERT_TRUE(sb.isEmpty());
    ASSERT_FALSE(sb.isFull());
    ASSERT_EQ(size_t(64), sb.freeSlotCount());

    std::set<int> claimed;
    for (size_t i = 0; i < 64; i++) {
        OccupancyTransition t{};
        int slot = sb.allocSlot(t);
        ASSERT_GE(slot, 0);
        ASSERT_LT(slot, 64);
        ASSERT_EQ(size_t(0), claimed.count(slot));
        claimed.insert(slot);
    }
    ASSERT_TRUE(sb.isFull());
    ASSERT_EQ(size_t(0), sb.freeSlotCount());

    // 65th alloc fails and reports a no-op transition (Full -> Full).
    OccupancyTransition t{};
    int slot = sb.allocSlot(t);
    ASSERT_EQ(-1, slot);
    ASSERT_EQ(OccupancyState::Full, t.before);
    ASSERT_EQ(OccupancyState::Full, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameAvailable());
}

TEST(SlabBookkeeper_SeedAllUsed_AllocFails) {
    SlabBookkeeper<64> sb;
    sb.seedAllUsed();
    ASSERT_TRUE(sb.isFull());
    ASSERT_FALSE(sb.isEmpty());

    OccupancyTransition t{};
    ASSERT_EQ(-1, sb.allocSlot(t));
    ASSERT_EQ(OccupancyState::Full, t.before);
    ASSERT_EQ(OccupancyState::Full, t.after);
}

// ============================================================
// Transition reporting — Empty→Partial→Full and the inverse
// ============================================================

TEST(SlabBookkeeper_Transition_EmptyToPartial) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();

    OccupancyTransition t{};
    int slot = sb.allocSlot(t);
    ASSERT_GE(slot, 0);
    ASSERT_EQ(OccupancyState::Empty, t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameFull());
    ASSERT_FALSE(t.becameEmpty());
    ASSERT_FALSE(t.becameAvailable());
}

TEST(SlabBookkeeper_Transition_PartialToFull) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    for (size_t i = 0; i < 63; i++) (void)sb.allocSlot(t);

    int slot = sb.allocSlot(t);
    ASSERT_GE(slot, 0);
    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Full, t.after);
    ASSERT_TRUE(t.becameFull());
}

TEST(SlabBookkeeper_Transition_FullToPartial_OnFree) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    // Allocate every slot, remember them.
    int slots[64];
    for (size_t i = 0; i < 64; i++) slots[i] = sb.allocSlot(t);
    ASSERT_TRUE(sb.isFull());

    sb.freeSlot(slots[0], t);
    ASSERT_EQ(OccupancyState::Full, t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_TRUE(t.becameAvailable());
    ASSERT_FALSE(t.becameEmpty());
}

TEST(SlabBookkeeper_Transition_PartialToEmpty_OnFree) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    int s0 = sb.allocSlot(t);
    int s1 = sb.allocSlot(t);

    sb.freeSlot(s0, t);
    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_FALSE(t.becameEmpty());

    sb.freeSlot(s1, t);
    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Empty, t.after);
    ASSERT_TRUE(t.becameEmpty());
}

// ============================================================
// allocSlots (multi-slot callback path)
// ============================================================

TEST(SlabBookkeeper_AllocSlots_Bulk) {
    SlabBookkeeper<128> sb;
    sb.seedAllAvailable();

    std::set<size_t> claimed;
    OccupancyTransition t{};
    const size_t got = sb.allocSlots(50,
                                     [&](size_t s) { claimed.insert(s); },
                                     t);
    ASSERT_EQ(size_t(50), got);
    ASSERT_EQ(size_t(50), claimed.size());
    ASSERT_EQ(OccupancyState::Empty,   t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);
    ASSERT_EQ(size_t(50), sb.allocatedSlotCount());
}

TEST(SlabBookkeeper_AllocSlots_Exhausted) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    const size_t got = sb.allocSlots(1000,
                                     [&](size_t) {},
                                     t);
    ASSERT_EQ(size_t(64), got);
    ASSERT_TRUE(sb.isFull());
    ASSERT_TRUE(t.becameFull());
}

// ============================================================
// Bulk free
// ============================================================

TEST(SlabBookkeeper_FreeSlotsBulk) {
    SlabBookkeeper<128> sb;
    sb.seedAllAvailable();

    // Allocate 100 slots.
    OccupancyTransition t{};
    int slots[100];
    for (size_t i = 0; i < 100; i++) slots[i] = sb.allocSlot(t);

    // Coalesce the first 64 slots' indices into per-word masks for bulk
    // release.
    constexpr size_t W = SlabBookkeeper<128>::kBitmapWordCount;
    uint64_t pending[W] = {};
    for (size_t i = 0; i < 64; i++) {
        const size_t s = static_cast<size_t>(slots[i]);
        pending[s / 64] |= 1ull << (s % 64);
    }
    sb.freeSlotsBulk(pending, 64, t);

    ASSERT_EQ(size_t(36), sb.allocatedSlotCount());
    ASSERT_FALSE(sb.isFull());
    ASSERT_FALSE(sb.isEmpty());
}

// ============================================================
// Reservation
// ============================================================

TEST(SlabBookkeeper_ReserveSlot_RemovesFromCirculation) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();

    sb.reserveSlot(7);
    sb.reserveSlot(42);
    ASSERT_TRUE(sb.hasReservedSlots());
    ASSERT_EQ(size_t(2), sb.reservedSlotCount());
    ASSERT_EQ(size_t(62), sb.freeSlotCount());

    // The remaining 62 slots can all be allocated.
    std::set<int> claimed;
    OccupancyTransition t{};
    for (size_t i = 0; i < 62; i++) {
        int s = sb.allocSlot(t);
        ASSERT_GE(s, 0);
        ASSERT_NE(s, 7);
        ASSERT_NE(s, 42);
        claimed.insert(s);
    }
    ASSERT_EQ(size_t(62), claimed.size());
    ASSERT_TRUE(sb.isFull());
    ASSERT_FALSE(sb.isEmpty()); // not "empty" once reservations exist
}

TEST(SlabBookkeeper_ReserveSlot_AlreadyReserved_NoOp) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    sb.reserveSlot(10);
    sb.reserveSlot(10); // double-reserve is a graceful no-op
    ASSERT_EQ(size_t(1), sb.reservedSlotCount());
}

// ============================================================
// External-storage construction
// ============================================================

TEST(SlabBookkeeper_ExternalStorage_Roundtrip) {
    alignas(64) uint64_t allocStorage[1] = {};
    alignas(64) Atomic<uint64_t> freeStorage[1]{};
    using ExtBookkeeper = SlabBookkeeper<64,
                                         ExternalSplitBitmapStorage<1>,
                                         /*UseHint=*/false>;
    ExtBookkeeper sb(allocStorage, freeStorage);
    sb.seedAllAvailable();
    ASSERT_TRUE(sb.isEmpty());
    ASSERT_EQ(size_t(64), sb.freeSlotCount());

    OccupancyTransition t{};
    int s = sb.allocSlot(t);
    ASSERT_GE(s, 0);
    sb.freeSlot(s, t);
    ASSERT_TRUE(sb.isEmpty());
}

// ============================================================
// Slab (full-fat) — address ↔ index translation
// ============================================================

TEST(Slab_AllocFree_Roundtrip) {
    alignas(64) uint8_t buffer[64 * 32];
    Slab<64, 32> slab(buffer);
    ASSERT_TRUE(slab.isEmpty());

    OccupancyTransition t{};
    void* p = slab.alloc(t);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(slab.contains(p));
    // First slot returned should be at base + 0.
    ASSERT_EQ(static_cast<void*>(buffer), p);
    ASSERT_EQ(OccupancyState::Empty, t.before);
    ASSERT_EQ(OccupancyState::Partial, t.after);

    slab.free(p, t);
    ASSERT_TRUE(slab.isEmpty());
    ASSERT_EQ(OccupancyState::Partial, t.before);
    ASSERT_EQ(OccupancyState::Empty, t.after);
}

TEST(Slab_AllSlotsUnique) {
    alignas(64) uint8_t buffer[64 * 16];
    Slab<64, 16> slab(buffer);

    std::set<void*> pointers;
    for (size_t i = 0; i < 64; i++) {
        void* p = slab.alloc();
        ASSERT_NE(p, nullptr);
        ASSERT_TRUE(slab.contains(p));
        ASSERT_EQ(size_t(0), pointers.count(p));
        pointers.insert(p);
    }
    ASSERT_TRUE(slab.isFull());
    ASSERT_EQ(nullptr, slab.alloc()); // exhausted

    // All pointers must be 16-byte spaced within the buffer.
    for (void* p : pointers) {
        const uintptr_t off =
            reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(buffer);
        ASSERT_EQ(uintptr_t(0), off % 16);
        ASSERT_LT(off, sizeof(buffer));
    }
}

TEST(Slab_Contains_BoundsCheck) {
    alignas(64) uint8_t buffer[64 * 8];
    Slab<64, 8> slab(buffer);
    ASSERT_TRUE(slab.contains(buffer));
    ASSERT_TRUE(slab.contains(buffer + sizeof(buffer) - 1));
    ASSERT_FALSE(slab.contains(buffer + sizeof(buffer))); // one past end
    ASSERT_FALSE(slab.contains(buffer - 1));              // one before start
    uint8_t other = 0;
    ASSERT_FALSE(slab.contains(&other));
}

// ============================================================
// DEC-011 — sub-word and non-multiple-of-64 SlotCounts
// ============================================================
//
// Parameterized helper exercised across the kWordCount equivalence classes
// {1, 2, 3 words} and at the boundaries (1, 63, 64, 65, 127, 128, 129, ...).
// Verifies: seedAllAvailable masks tail bits, allocSlot only returns indices
// in [0, SlotCount), exhaustion is clean, free returns the slab to isEmpty.

template <size_t N>
static void testSeedAndExhaust() {
    SlabBookkeeper<N> sb;
    sb.seedAllAvailable();

    ASSERT_TRUE(sb.isEmpty());
    ASSERT_FALSE(sb.isFull());
    ASSERT_EQ(size_t(N), sb.freeSlotCount());
    ASSERT_EQ(size_t(N), sb.bitmapAvailableCount());

    std::set<int> claimed;
    OccupancyTransition t{};
    for (size_t i = 0; i < N; i++) {
        int slot = sb.allocSlot(t);
        ASSERT_GE(slot, 0);
        ASSERT_LT(static_cast<size_t>(slot), N);
        ASSERT_EQ(size_t(0), claimed.count(slot));
        claimed.insert(slot);
    }
    ASSERT_EQ(size_t(N), claimed.size());
    ASSERT_TRUE(sb.isFull());
    ASSERT_EQ(size_t(0), sb.freeSlotCount());

    // Exhaustion: next claim returns -1 with Full→Full transition.
    int extra = sb.allocSlot(t);
    ASSERT_EQ(-1, extra);
    ASSERT_EQ(OccupancyState::Full, t.before);
    ASSERT_EQ(OccupancyState::Full, t.after);

    // Free everything and confirm we land back at isEmpty().
    for (int slot : claimed) sb.freeSlot(static_cast<size_t>(slot), t);
    ASSERT_TRUE(sb.isEmpty());
    ASSERT_EQ(size_t(N), sb.freeSlotCount());
}

TEST(SlabBookkeeper_SubWord_SlotCount_1)   { testSeedAndExhaust<1>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_7)   { testSeedAndExhaust<7>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_15)  { testSeedAndExhaust<15>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_63)  { testSeedAndExhaust<63>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_65)  { testSeedAndExhaust<65>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_127) { testSeedAndExhaust<127>(); }
TEST(SlabBookkeeper_SubWord_SlotCount_137) { testSeedAndExhaust<137>(); }

// seedAllAvailable(usableCount) variant: explicit shrink below SlotCount.
// The usable region is [0, usableCount); the bookkeeper masks bits in
// [usableCount, kWordCount*64) via reserveSlot. After shrinkage the slab is
// NOT isEmpty() — caller-imposed reservations count past kTailBits.
TEST(SlabBookkeeper_SeedAllAvailable_UsableCountShrink) {
    SlabBookkeeper<128> sb;
    sb.seedAllAvailable(50);
    ASSERT_EQ(size_t(50), sb.freeSlotCount());
    ASSERT_EQ(size_t(50), sb.bitmapAvailableCount());
    // 78 bits in [50, 128) are reserveSlot'd; only 0 tail bits exist for
    // SlotCount=128, so reservedCount = 78 > kTailBits = 0.
    ASSERT_EQ(size_t(78), sb.reservedSlotCount());
    ASSERT_FALSE(sb.isEmpty());

    OccupancyTransition t{};
    std::set<int> claimed;
    for (size_t i = 0; i < 50; i++) {
        int slot = sb.allocSlot(t);
        ASSERT_GE(slot, 0);
        ASSERT_LT(slot, 50);
        claimed.insert(slot);
    }
    ASSERT_EQ(size_t(50), claimed.size());
    ASSERT_TRUE(sb.isFull());
    ASSERT_EQ(-1, sb.allocSlot(t));
}

// Caller reserveSlot composes with structural tail masking: reservedCount
// after seeding is kTailBits; subsequent caller reserveSlot grows it; the
// usable region shrinks accordingly without disturbing isEmpty semantics.
TEST(SlabBookkeeper_SubWord_ReserveSlot_Composes) {
    SlabBookkeeper<65> sb;
    sb.seedAllAvailable();
    // After seeding, reservedCount == kTailBits == 63; isEmpty true.
    ASSERT_TRUE(sb.isEmpty());
    ASSERT_EQ(size_t(63), sb.reservedSlotCount());
    ASSERT_EQ(size_t(65), sb.freeSlotCount());

    sb.reserveSlot(7);
    ASSERT_FALSE(sb.isEmpty());           // caller reservation breaks "empty"
    ASSERT_EQ(size_t(64), sb.freeSlotCount());

    // Allocate the remaining 64 usable slots — none should be slot 7.
    OccupancyTransition t{};
    for (size_t i = 0; i < 64; i++) {
        int slot = sb.allocSlot(t);
        ASSERT_GE(slot, 0);
        ASSERT_NE(7, slot);
        ASSERT_LT(slot, 65);
    }
    ASSERT_TRUE(sb.isFull());
}

// ============================================================
// DEC-013 — double-free detection propagation through SlabBookkeeper
// ============================================================

TEST(SlabBookkeeper_DoubleFree_Asserts) {
    SlabBookkeeper<64> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    int slot = sb.allocSlot(t);
    ASSERT_GE(slot, 0);
    sb.freeSlot(static_cast<size_t>(slot), t);

    // The second free of the same slot must trip SplitBitmap::releaseBit's
    // double-free assert, which the test harness surfaces as an exception.
    EXPECT_ASSERT_FAILURE(sb.freeSlot(static_cast<size_t>(slot), t));
}

TEST(SlabBookkeeper_DoubleFreeBulk_Asserts) {
    SlabBookkeeper<128> sb;
    sb.seedAllAvailable();
    OccupancyTransition t{};
    // Allocate two slots.
    int s0 = sb.allocSlot(t);
    int s1 = sb.allocSlot(t);
    ASSERT_GE(s0, 0);
    ASSERT_GE(s1, 0);

    // Free both via bulk.
    constexpr size_t W = SlabBookkeeper<128>::kBitmapWordCount;
    uint64_t pending[W] = {};
    pending[s0 / 64] |= uint64_t{1} << (s0 % 64);
    pending[s1 / 64] |= uint64_t{1} << (s1 % 64);
    sb.freeSlotsBulk(pending, 2, t);

    // Re-issuing the same bulk-free mask must trip releaseBitsBulk's assert.
    EXPECT_ASSERT_FAILURE(sb.freeSlotsBulk(pending, 2, t));
}
