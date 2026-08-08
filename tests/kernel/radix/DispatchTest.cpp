//
// radix-tree Phase 1 — the §6.3 write dispatch, all eight rows.
//
// A pure function, so this file needs no tree at all: it builds slot words by
// hand and asks what the dispatch would do. That is the whole reason §6.3 is
// stated as a function of slot content rather than as steps inside an
// operation — the classification is fully decidable before any concurrency
// exists, and Phase 2 changes none of it.
//
// The rows with history get the most attention:
//
//   - the MIDDLE PUNCH, which satisfied the shrink row's letter while
//     satisfying nobody's action until DEC-066 re-routed it;
//   - the EMPTY-CLEAR no-op, whose wrong answer is a spurious fetch_sub that is
//     silent in release;
//   - the EXPRESSIBILITY boundary between the shrink row and subdivision, which
//     must be exact in both directions.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/Geometry.h>
#include <mem/radix/SlotCodec.h>
#include <mem/radix/Dispatch.h>
#include <mem/radix/Mapping.h>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using Codec = rdx::HarnessSlotCodec<GA>;

// Two distinct 16 B-aligned addresses inside the arena, standing in for a
// Mapping and a node. The dispatch never dereferences either.
void* recordA() {
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(VS::arenaVirtualBase(0).value) + 0x1000);
}
void* recordB() {
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(VS::arenaVirtualBase(0).value) + 0x2000);
}
void* nodeAddr() {
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(VS::arenaVirtualBase(0).value) + 0x3000);
}

rdx::DispatchResult run(uint64_t word, unsigned level, uint64_t slotBase,
                        uint64_t lo, uint64_t hi, bool writes) {
    return rdx::dispatchSlot<GA, Codec>(word, level, slotBase, lo, hi, writes);
}

// C2 under the amd64 geometry: 64 KiB slots, 4 KiB range units (shortfall 0), so
// every page-granular boundary is expressible. The level where the shrink row is
// reachable.
constexpr unsigned kC2 = 5;
constexpr uint64_t kC2Span = uint64_t{64} * 1024;

// C0: 32 MiB slots, 8 KiB range units (shortfall 1). The level where a 4 KiB
// boundary is NOT expressible and forces subdivision.
constexpr unsigned kC0 = 3;
constexpr uint64_t kC0Span = uint64_t{32} * 1024 * 1024;

}  // namespace

// ─── Rows 1 and 8: the empty slot ──────────────────────────────────────────

TEST(radix_dispatch_empty_slot_writing_writes_a_leaf) {
    Harness h;
    auto r = run(0, kC2, 0, 0, kC2Span - 1, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::WriteLeaf);
    ASSERT_TRUE(r.range == Codec::fullSpan(kC2));
    ASSERT_EQ(1, rdx::occupancyDelta(r.action));
    ASSERT_FALSE(rdx::displacesNamedRecord(r.action));
}

TEST(radix_dispatch_empty_slot_partially_written_gets_a_sub_range_leaf) {
    Harness h;
    // §6.5: "a partially-intersected empty slot ... for a map, gets an
    // edge-partial sub-range leaf ... writing the full span would map addresses
    // the caller never asked for."
    auto r = run(0, kC2, 0, 0, 4095, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::WriteLeaf);
    ASSERT_TRUE(r.range == (rdx::SubRange{0, 0}));   // one 4 KiB unit, not the whole 64 KiB
}

TEST(radix_dispatch_empty_slot_clearing_is_a_no_op_with_no_count_change) {
    Harness h;
    // §6.3's eighth row, added by DEC-077. Reached by a decomposed munmap over a
    // range containing holes. The wrong answer here is a spurious fetch_sub on
    // an already-zero count, whose borrow propagates into the spare bits — and
    // which, with the mark placed above the count instead of below it, would
    // have cleared a dying mark. It is silent in release.
    auto r = run(0, kC2, 0, 0, kC2Span - 1, /*writes=*/false);
    ASSERT_TRUE(r.action == rdx::DispatchAction::NoOp);
    ASSERT_EQ(0, rdx::occupancyDelta(r.action));
    ASSERT_FALSE(rdx::displacesNamedRecord(r.action));

    // Partially intersected, too — likewise a no-op, not a partial clear.
    auto r2 = run(0, kC2, 0, 8192, 12287, /*writes=*/false);
    ASSERT_TRUE(r2.action == rdx::DispatchAction::NoOp);
    ASSERT_EQ(0, rdx::occupancyDelta(r2.action));
}

// ─── Rows 2 and 7: a fully covered leaf ────────────────────────────────────

TEST(radix_dispatch_full_leaf_overwrite_and_clear) {
    Harness h;
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC2), kC2);

    auto w = run(leaf, kC2, 0, 0, kC2Span - 1, /*writes=*/true);
    ASSERT_TRUE(w.action == rdx::DispatchAction::OverwriteLeaf);
    ASSERT_TRUE(rdx::displacesNamedRecord(w.action));    // the displaced record's -1
    ASSERT_EQ(0, rdx::occupancyDelta(w.action));         // occupied -> occupied
    ASSERT_EQ(0u, w.survivorCount);

    auto c = run(leaf, kC2, 0, 0, kC2Span - 1, /*writes=*/false);
    ASSERT_TRUE(c.action == rdx::DispatchAction::ClearSlot);
    ASSERT_TRUE(rdx::displacesNamedRecord(c.action));
    ASSERT_EQ(-1, rdx::occupancyDelta(c.action));
}

TEST(radix_dispatch_a_sub_range_leaf_fully_covered_by_a_wider_write) {
    Harness h;
    // The leaf covers [8K,16K) inside a 64 KiB slot; the write covers
    // [4K,20K) — strictly wider. No survivors, so one store, and the resulting
    // leaf covers the WRITE's range, not the old leaf's.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), rdx::SubRange{2, 3}, kC2);
    auto r = run(leaf, kC2, 0, 4096, 20479, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::OverwriteLeaf);
    ASSERT_EQ(0u, r.survivorCount);
    ASSERT_TRUE(r.range == (rdx::SubRange{1, 4}));
}

// ─── Row 6: the in-place shrink ────────────────────────────────────────────

TEST(radix_dispatch_edge_partial_munmap_shrinks_in_place) {
    Harness h;
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC2), kC2);

    // Clear the first 4 KiB: one survivor, expressible at C2's 4 KiB unit.
    auto lead = run(leaf, kC2, 0, 0, 4095, /*writes=*/false);
    ASSERT_TRUE(lead.action == rdx::DispatchAction::ShrinkLeafInPlace);
    ASSERT_EQ(1u, lead.survivorCount);
    ASSERT_TRUE(lead.range == (rdx::SubRange{1, 15}));
    // ±0 — the slot still names the same record. The single most important
    // negative in the table: this is the one slot rewrite that must not touch
    // the count, and a spurious decrement here is indistinguishable from the
    // correct case until the record is freed early.
    ASSERT_FALSE(rdx::displacesNamedRecord(lead.action));
    ASSERT_EQ(0, rdx::occupancyDelta(lead.action));

    // ...and the trailing edge.
    auto tail = run(leaf, kC2, 0, kC2Span - 4096, kC2Span - 1, /*writes=*/false);
    ASSERT_TRUE(tail.action == rdx::DispatchAction::ShrinkLeafInPlace);
    ASSERT_TRUE(tail.range == (rdx::SubRange{0, 14}));
}

TEST(radix_dispatch_a_clear_missing_the_leaf_entirely_is_a_no_op) {
    Harness h;
    // The leaf covers [8K,16K); the clear covers [32K,36K) — same slot, no
    // overlap. Nothing to clear, and nothing to decrement. This is the hole case
    // inside an OCCUPIED slot, distinct from the empty-slot hole case.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), rdx::SubRange{2, 3}, kC2);
    auto r = run(leaf, kC2, 0, 32768, 36863, /*writes=*/false);
    ASSERT_TRUE(r.action == rdx::DispatchAction::NoOp);
    ASSERT_EQ(0, rdx::occupancyDelta(r.action));
}

// ─── Row 3: subdivision ────────────────────────────────────────────────────

TEST(radix_dispatch_middle_punch_munmap_subdivides) {
    Harness h;
    // DEC-066's case. Every boundary here IS expressible at C2's 4 KiB unit, so
    // the shrink row's LETTER is satisfied — and its action is not, because one
    // slot cannot hold two ranges (invariant 3). Before DEC-066 an implementer
    // following the table either left the punched page mapped (munmap returning
    // success on a still-accessible address) or dropped a survivor.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC2), kC2);
    auto r = run(leaf, kC2, 0, 8192, 12287, /*writes=*/false);

    ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
    ASSERT_EQ(2u, r.survivorCount);
    ASSERT_TRUE(r.survivors[0] == (rdx::Interval{0, 8191}));
    ASSERT_TRUE(r.survivors[1] == (rdx::Interval{12288, kC2Span - 1}));
}

TEST(radix_dispatch_single_survivor_but_inexpressible_subdivides) {
    Harness h;
    // At C0 the range unit is 8 KiB, so a 4 KiB (POSIX-legal, page-granular)
    // boundary cannot be expressed. One survivor, and still a subdivision —
    // §6.1's shortfall, which is why the allocation bound is level-dependent
    // rather than flat.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC0), kC0);
    auto r = run(leaf, kC0, 0, 0, 4095, /*writes=*/false);
    ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
    ASSERT_EQ(1u, r.survivorCount);
    ASSERT_TRUE(r.survivors[0] == (rdx::Interval{4096, kC0Span - 1}));

    // The same clear one level down IS expressible, so it shrinks — the
    // shortfall is exactly one level, as the geometry derives.
    const uint64_t c1Leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(4), 4);
    auto r2 = run(c1Leaf, 4, 0, 0, 4095, /*writes=*/false);
    ASSERT_TRUE(r2.action == rdx::DispatchAction::ShrinkLeafInPlace);
}

TEST(radix_dispatch_edge_mmap_into_an_occupied_leaf_subdivides) {
    Harness h;
    // §7.2's edge-subdivision row: the child ends up holding the survivor AND
    // the new record. Same dispatch row as the middle punch, opposite reference
    // delta — which is exactly why §7.2 is stated over naming-slot transitions
    // rather than over an enumeration of events.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC2), kC2);
    auto r = run(leaf, kC2, 0, 0, 4095, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
    ASSERT_EQ(1u, r.survivorCount);
    ASSERT_TRUE(r.survivors[0] == (rdx::Interval{4096, kC2Span - 1}));
    ASSERT_TRUE(r.opRange == (rdx::Interval{0, 4095}));
}

TEST(radix_dispatch_middle_mmap_into_an_occupied_leaf_subdivides_with_two_survivors) {
    Harness h;
    const uint64_t leaf = Codec::encodeLeaf(recordA(), Codec::fullSpan(kC2), kC2);
    auto r = run(leaf, kC2, 0, 8192, 12287, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
    ASSERT_EQ(2u, r.survivorCount);
    // Three segments in the plan: survivor, new value, survivor. The minimal
    // three-leaf shape §6.3 names for a middle MAP_FIXED.
    ASSERT_TRUE(r.opRange == (rdx::Interval{8192, 12287}));
}

TEST(radix_dispatch_write_into_an_uncovered_part_of_an_occupied_slot_subdivides) {
    Harness h;
    // Leaf covers [8K,16K); the write goes to [32K,36K) in the same slot. The
    // ranges are disjoint, so there is no "displacement" at all — but two values
    // still want one slot, so it subdivides and the survivor is the WHOLE
    // existing leaf.
    const uint64_t leaf = Codec::encodeLeaf(recordA(), rdx::SubRange{2, 3}, kC2);
    auto r = run(leaf, kC2, 0, 32768, 36863, /*writes=*/true);
    ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
    ASSERT_EQ(1u, r.survivorCount);
    ASSERT_TRUE(r.survivors[0] == (rdx::Interval{8192, 16383}));
}

// ─── Rows 4 and 5: a child slot ────────────────────────────────────────────

TEST(radix_dispatch_child_slot_descends_or_detaches_on_slot_coverage) {
    Harness h;
    const uint64_t child = Codec::encodeChild(nodeAddr());

    // Fully covers the SLOT's span -> detach. Note the test is against the slot
    // span, not against whatever the child contains.
    auto full = run(child, kC2, 0, 0, kC2Span - 1, /*writes=*/false);
    ASSERT_TRUE(full.action == rdx::DispatchAction::DetachChild);

    // Anything less -> descend. Never subdivide: subdividing here would detach a
    // live interior node.
    auto part = run(child, kC2, 0, 0, kC2Span - 4097, /*writes=*/false);
    ASSERT_TRUE(part.action == rdx::DispatchAction::DescendIntoChild);
    auto one = run(child, kC2, 0, 4096, 8191, /*writes=*/true);
    ASSERT_TRUE(one.action == rdx::DispatchAction::DescendIntoChild);

    // The writing variant of full coverage is still a detach — "store the new
    // value, retire the subtree".
    auto fullWrite = run(child, kC2, 0, 0, kC2Span - 1, /*writes=*/true);
    ASSERT_TRUE(fullWrite.action == rdx::DispatchAction::DetachChild);

    // A detached subtree's displaced values ride the node deleters, so the
    // parent takes no per-record decrement here.
    ASSERT_FALSE(rdx::displacesNamedRecord(full.action));
}

// ─── The rows are exhaustive ───────────────────────────────────────────────

TEST(radix_dispatch_every_input_shape_lands_on_a_row) {
    Harness h;
    // Sweep every (slot content x operation range x direction) at a level small
    // enough to enumerate, and assert the classification is total and internally
    // consistent. "Exhaustive" is the claim §6.3 makes; this is what checks it.
    constexpr auto GT = rdx::kTinyGeometry;
    using TC = rdx::HarnessSlotCodec<GT>;
    constexpr unsigned level = 2;
    const uint64_t span = rdx::slotSpan(GT, level);
    const uint64_t unit = uint64_t{1} << GT.floorBits;
    const uint64_t units = rdx::rangeUnitCount(GT, level);

    // Slot contents: empty, a child, and every representable leaf range.
    for (unsigned contentIdx = 0; contentIdx < units * units + 2; contentIdx++) {
        uint64_t word;
        if (contentIdx == 0) {
            word = 0;
        } else if (contentIdx == 1) {
            word = TC::encodeChild(nodeAddr());
        } else {
            const uint64_t k = contentIdx - 2;
            const uint32_t lo = static_cast<uint32_t>(k / units);
            const uint32_t hi = static_cast<uint32_t>(k % units);
            if (hi < lo) continue;
            word = TC::encodeLeaf(recordA(), rdx::SubRange{lo, hi}, level);
        }

        for (uint64_t opLo = 0; opLo < span; opLo += unit) {
            for (uint64_t opHi = opLo + unit - 1; opHi < span; opHi += unit) {
                for (bool writes : { false, true }) {
                    auto r = rdx::dispatchSlot<GT, TC>(word, level, 0, opLo, opHi, writes);

                    // Every action is reachable only from the slot kind it
                    // belongs to. A misclassification here is the row-confusion
                    // DEC-066 fixed.
                    switch (TC::kindOf(word)) {
                    case rdx::SlotKind::Empty:
                        ASSERT_TRUE(r.action == rdx::DispatchAction::NoOp ||
                                    r.action == rdx::DispatchAction::WriteLeaf ||
                                    r.action == rdx::DispatchAction::Subdivide);
                        ASSERT_EQ(0u, r.survivorCount);
                        break;
                    case rdx::SlotKind::Child:
                        ASSERT_TRUE(r.action == rdx::DispatchAction::DescendIntoChild ||
                                    r.action == rdx::DispatchAction::DetachChild);
                        break;
                    case rdx::SlotKind::Leaf:
                        ASSERT_TRUE(r.action == rdx::DispatchAction::NoOp ||
                                    r.action == rdx::DispatchAction::OverwriteLeaf ||
                                    r.action == rdx::DispatchAction::ClearSlot ||
                                    r.action == rdx::DispatchAction::ShrinkLeafInPlace ||
                                    r.action == rdx::DispatchAction::Subdivide);
                        break;
                    }

                    // A clearing operation NEVER increases occupancy and never
                    // writes a value; a writing one never decreases it.
                    if (writes) ASSERT_TRUE(rdx::occupancyDelta(r.action) >= 0);
                    else        ASSERT_TRUE(rdx::occupancyDelta(r.action) <= 0);

                    // The no-op row is the only one that touches nothing, and it
                    // is reachable only when there is genuinely nothing to do.
                    if (r.action == rdx::DispatchAction::NoOp) {
                        ASSERT_FALSE(writes);
                        ASSERT_EQ(0, rdx::occupancyDelta(r.action));
                        ASSERT_FALSE(rdx::displacesNamedRecord(r.action));
                    }

                    // Two survivors always subdivide, whatever their
                    // expressibility — invariant 3, one contiguous sub-range per
                    // leaf.
                    if (r.survivorCount == 2) {
                        ASSERT_TRUE(r.action == rdx::DispatchAction::Subdivide);
                    }
                }
            }
        }
    }
}
