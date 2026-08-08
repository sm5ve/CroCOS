//
// radix-tree Phase 1 — the slot codec (§5.2; DEC-021/022/023/025/081).
//
// §11's target: "Every codec round-trips and rejects out-of-range input —
// exhaustive encode/decode per level, plus an explicit case for a node at
// VMSubstrate offset 0 under the compressed codec, asserting DEC-081's guard bit
// keeps the word nonzero and round-tripping; plus the empty-range reject in
// both codecs."
//
// The offset-0 case is the one worth spelling out. It is not a corner: it is the
// exact collision that made the guard bit necessary, and before DEC-081 an
// implementation could pass every other codec test while a node allocated at the
// bottom of the window encoded to all-zero and read back as an EMPTY SLOT — a
// whole subtree silently unreachable, with the parent's occupancy count still
// saying it was there.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/Geometry.h>
#include <mem/radix/SlotCodec.h>
#include <mem/radix/Mapping.h>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

using TinyCodec  = rdx::HarnessSlotCodec<rdx::kTinyGeometry>;
using Amd64Codec = rdx::HarnessSlotCodec<rdx::kAmd64Geometry>;

// A 16 B-aligned address inside the window, `n` slots up from the base.
void* addrAt(uint64_t offsetBytes) {
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(VS::arenaVirtualBase(0).value) +
        static_cast<uintptr_t>(offsetBytes));
}

}  // namespace

// ─── Classification ────────────────────────────────────────────────────────

TEST(radix_codec_three_slot_states_are_exhaustive) {
    Harness h;

    ASSERT_TRUE(TinyCodec::isEmpty(0));
    ASSERT_FALSE(TinyCodec::isLeaf(0));
    ASSERT_FALSE(TinyCodec::isChild(0));
    ASSERT_TRUE(TinyCodec::kindOf(0) == rdx::SlotKind::Empty);

    const uint64_t child = TinyCodec::encodeChild(addrAt(64));
    ASSERT_TRUE(TinyCodec::isChild(child));
    ASSERT_FALSE(TinyCodec::isLeaf(child));
    ASSERT_FALSE(TinyCodec::isEmpty(child));

    const uint64_t leaf = TinyCodec::encodeLeaf(addrAt(128), rdx::SubRange{0, 0}, 3);
    ASSERT_TRUE(TinyCodec::isLeaf(leaf));
    ASSERT_FALSE(TinyCodec::isChild(leaf));
    ASSERT_FALSE(TinyCodec::isEmpty(leaf));
}

// ─── DEC-081(a): the guard bit, and the offset-0 collision ─────────────────

TEST(radix_codec_child_at_window_offset_zero_is_not_the_empty_word) {
    Harness h;

    // The exact case the guard bit exists for: an object at the very bottom of
    // the window. Without kChildGuardBit this encodes to all-zero.
    void* atZero = addrAt(0);
    const uint64_t word = TinyCodec::encodeChild(atZero);

    ASSERT_TRUE(word != 0);
    ASSERT_TRUE(TinyCodec::isChild(word));
    ASSERT_FALSE(TinyCodec::isEmpty(word));
    ASSERT_EQ(atZero, TinyCodec::decodeChild(word));
    // And the bit is exactly the one DEC-081 names, not some other tell.
    ASSERT_TRUE((word & rdx::kChildGuardBit) != 0);
}

TEST(radix_codec_guard_bit_is_masked_off_on_decode) {
    Harness h;
    for (uint64_t off = 0; off < 4096; off += 16) {
        void* p = addrAt(off);
        const uint64_t w = Amd64Codec::encodeChild(p);
        ASSERT_EQ(p, Amd64Codec::decodeChild(w));
        ASSERT_TRUE(w != 0);
    }
}

// ─── Round-trip, exhaustively per level ────────────────────────────────────

TEST(radix_codec_round_trips_every_range_at_every_tiny_level) {
    Harness h;
    void* m = addrAt(256);

    // Tiny geometry: small enough that "every level x every (lo, hi) pair" is a
    // genuine exhaustive sweep rather than a sample.
    for (unsigned level = 1; level <= rdx::kTinyGeometry.levelCount; level++) {
        const uint64_t units = rdx::rangeUnitCount(rdx::kTinyGeometry, level);
        for (uint64_t lo = 0; lo < units; lo++) {
            for (uint64_t hi = lo; hi < units; hi++) {
                const rdx::SubRange r{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi)};
                const uint64_t w = TinyCodec::encodeLeaf(m, r, level);
                ASSERT_TRUE(TinyCodec::isLeaf(w));
                ASSERT_EQ(m, TinyCodec::decodeLeaf(w));
                ASSERT_TRUE(TinyCodec::decodeRange(w, level) == r);
            }
        }
    }
}

TEST(radix_codec_round_trips_range_extremes_at_every_amd64_level) {
    Harness h;
    void* m = addrAt(512);

    // The real geometry's unit counts run to 4096, so sweep the extremes and a
    // sample rather than the full cross product.
    for (unsigned level = 1; level <= rdx::kAmd64Geometry.levelCount; level++) {
        const uint64_t units = rdx::rangeUnitCount(rdx::kAmd64Geometry, level);
        const uint64_t probes[] = { 0, 1, units / 3, units / 2, units - 2, units - 1 };
        for (uint64_t lo : probes) {
            for (uint64_t hi : probes) {
                if (hi < lo || hi >= units) continue;
                const rdx::SubRange r{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi)};
                const uint64_t w = Amd64Codec::encodeLeaf(m, r, level);
                ASSERT_EQ(m, Amd64Codec::decodeLeaf(w));
                ASSERT_TRUE(Amd64Codec::decodeRange(w, level) == r);
            }
        }
        // The full-span leaf must be representable at every level — this is the
        // off-by-one DEC-039 flagged (ITEM-049): a half-open range over N units
        // needs N+1 end values and would NOT fit the budget.
        const rdx::SubRange full = Amd64Codec::fullSpan(level);
        const uint64_t w = Amd64Codec::encodeLeaf(m, full, level);
        ASSERT_TRUE(Amd64Codec::decodeRange(w, level) == full);
        ASSERT_EQ(units - 1, full.hi);
    }
}

TEST(radix_codec_pointer_and_range_do_not_alias) {
    Harness h;
    // Two leaves differing only in range must differ only in the range field,
    // and two differing only in pointer only in the pointer field. An overlap
    // between the fields is silent — it corrupts an address or a range and both
    // read back as plausible values.
    void* a = addrAt(1024);
    void* b = addrAt(1024 + 16);
    const unsigned level = 4;
    const uint64_t w1 = Amd64Codec::encodeLeaf(a, rdx::SubRange{0, 0}, level);
    const uint64_t w2 = Amd64Codec::encodeLeaf(a, rdx::SubRange{1, 1}, level);
    const uint64_t w3 = Amd64Codec::encodeLeaf(b, rdx::SubRange{0, 0}, level);

    ASSERT_EQ(a, Amd64Codec::decodeLeaf(w1));
    ASSERT_EQ(a, Amd64Codec::decodeLeaf(w2));
    ASSERT_EQ(b, Amd64Codec::decodeLeaf(w3));
    ASSERT_TRUE(Amd64Codec::decodeRange(w1, level) == (rdx::SubRange{0, 0}));
    ASSERT_TRUE(Amd64Codec::decodeRange(w3, level) == (rdx::SubRange{0, 0}));
}

// ─── DEC-081(b): the empty-range reject ────────────────────────────────────

TEST(radix_codec_rejects_an_empty_range) {
    Harness h;
    void* m = addrAt(2048);
    // The inclusive-end encoding cannot express an empty range and never needs
    // to: a slot that covers nothing IS the empty word. Encoding one is a
    // protocol violation, debug-asserted rather than given a reserved encoding
    // that nothing would consume.
    EXPECT_ASSERT_FAILURE(TinyCodec::encodeLeaf(m, rdx::SubRange{1, 0}, 3));
    EXPECT_ASSERT_FAILURE(Amd64Codec::encodeLeaf(m, rdx::SubRange{5, 4}, 4));
}

TEST(radix_codec_rejects_an_out_of_range_endpoint) {
    Harness h;
    void* m = addrAt(2048);
    const unsigned level = 3;
    const uint64_t units = rdx::rangeUnitCount(rdx::kTinyGeometry, level);
    EXPECT_ASSERT_FAILURE(
        TinyCodec::encodeLeaf(m, rdx::SubRange{0, static_cast<uint32_t>(units)}, level));
}

TEST(radix_codec_rejects_a_misaligned_pointer) {
    Harness h;
    // The tag occupies bits 3:0, which is free ONLY because every tree object is
    // at least 16 B aligned. An 8 B-aligned pointer would silently have its low
    // address bits eaten by the tag.
    EXPECT_ASSERT_FAILURE(TinyCodec::encodeChild(addrAt(8)));
}

TEST(radix_codec_rejects_a_pointer_outside_the_window) {
    Harness h;
    // §10: "the compressed codecs encode a VAS-layout fact... Each codec must
    // assert its own base and range at construction rather than trust
    // vas-layout.md to stay true." This is that assert doing its job.
    // One slot-alignment past the end of the arena: inside the NOMINAL 512 GiB
    // window (so the window check alone would wave it through, which is exactly
    // why the arena limit exists) but outside the region this codec is bound to.
    void* pastEnd = addrAt(kMockArenaBytes);
    EXPECT_ASSERT_FAILURE(TinyCodec::encodeChild(pastEnd));
}

// ─── `covers` and absolute ranges ──────────────────────────────────────────

TEST(radix_codec_covers_answers_unmapped_without_touching_the_mapping) {
    Harness h;
    void* m = addrAt(4096);
    const unsigned level = 3;   // tiny geometry: 4 B slots, 4 B units -> 1 unit
    const uint64_t span = rdx::slotSpan(rdx::kTinyGeometry, level);

    const uint64_t w = TinyCodec::encodeLeaf(m, rdx::SubRange{0, 0}, level);
    for (uint64_t k = 0; k < span; k++) ASSERT_TRUE(TinyCodec::covers(w, k, level));

    // A coarser level, where a sub-range genuinely leaves part of the slot
    // uncovered — the case that makes `covers` more than a tag check.
    const unsigned coarse = 1;
    const uint64_t units = rdx::rangeUnitCount(rdx::kTinyGeometry, coarse);
    const uint64_t unitSize = uint64_t{1} << rdx::rangeUnitBits(rdx::kTinyGeometry, coarse);
    ASSERT_TRUE(units >= 4);
    const uint64_t w2 = TinyCodec::encodeLeaf(m, rdx::SubRange{1, 2}, coarse);
    ASSERT_FALSE(TinyCodec::covers(w2, 0, coarse));
    ASSERT_TRUE (TinyCodec::covers(w2, unitSize, coarse));
    ASSERT_TRUE (TinyCodec::covers(w2, 2 * unitSize + 1, coarse));
    ASSERT_FALSE(TinyCodec::covers(w2, 3 * unitSize, coarse));

    // An empty word and a child word cover nothing — `covers` must not be
    // reachable as a tag-agnostic range test.
    ASSERT_FALSE(TinyCodec::covers(0, 0, coarse));
    ASSERT_FALSE(TinyCodec::covers(TinyCodec::encodeChild(addrAt(64)), 0, coarse));
}

TEST(radix_codec_absolute_range_and_sub_range_are_inverses) {
    Harness h;
    void* m = addrAt(8192);

    for (unsigned level = 1; level <= rdx::kTinyGeometry.levelCount; level++) {
        const uint64_t units = rdx::rangeUnitCount(rdx::kTinyGeometry, level);
        const uint64_t slotBase = 4 * rdx::slotSpan(rdx::kTinyGeometry, level);
        for (uint64_t lo = 0; lo < units; lo++) {
            for (uint64_t hi = lo; hi < units; hi++) {
                const rdx::SubRange r{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi)};
                const uint64_t w = TinyCodec::encodeLeaf(m, r, level);
                uint64_t alo = 0, ahi = 0;
                TinyCodec::absoluteRange(w, slotBase, level, alo, ahi);

                rdx::SubRange back{};
                ASSERT_TRUE(TinyCodec::subRangeFor(slotBase, level, alo, ahi, back));
                ASSERT_TRUE(back == r);
            }
        }
    }
}

TEST(radix_codec_sub_range_refuses_inexpressible_boundaries) {
    Harness h;
    // The expressibility test the §6.3 shrink row turns on. It must be EXACT in
    // both directions: rounding a survivor outward maps addresses the caller
    // never asked for, rounding it inward unmaps live ones. So a boundary that
    // is not unit-aligned must be refused, not approximated.
    const unsigned level = 3;   // amd64 C0: 8 KiB units inside a 32 MiB slot
    const uint64_t slotBase = 0;
    const uint64_t unit = uint64_t{1} << rdx::rangeUnitBits(rdx::kAmd64Geometry, level);
    ASSERT_EQ(uint64_t{8} * 1024, unit);

    rdx::SubRange out{};
    // Unit-aligned: expressible.
    ASSERT_TRUE(Amd64Codec::subRangeFor(slotBase, level, 0, unit - 1, out));
    ASSERT_TRUE(Amd64Codec::subRangeFor(slotBase, level, unit, 3 * unit - 1, out));
    // A 4 KiB (page-granular, POSIX-legal) boundary inside an 8 KiB unit: NOT
    // expressible at C0. This is the shortfall that forces one subdivision.
    ASSERT_FALSE(Amd64Codec::subRangeFor(slotBase, level, 0, unit / 2 - 1, out));
    ASSERT_FALSE(Amd64Codec::subRangeFor(slotBase, level, unit / 2, unit - 1, out));
    // ...and is expressible one level down, which is what shortfall == 1 means.
    ASSERT_TRUE(Amd64Codec::subRangeFor(slotBase, 4, 0, unit / 2 - 1, out));
}
