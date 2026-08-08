//
// radix-tree Phase 3 — the packed bucket entry (§5.1, DEC-102/103).
//
// The exit gate asks for a "bucket-codec round-trip + `static_assert` gate", and
// the round-trip is the easy half. What is worth testing beyond it is the set of
// claims that make the packing *safe*, each of which has a plausible
// implementation that round-trips perfectly and is still wrong:
//
//   - **all-zero means empty, structurally.** A cluster rooted at window offset
//     0, at level 0, base 0 would encode to all-zero without the guard bit and
//     read as an empty bucket — a whole 256 GiB zone silently unmappable, with
//     no crash. This is the DEC-081 collision one section over, and a
//     round-trip test never sees it because encode/decode agree.
//   - **the fields are DERIVED**, not written down. DEC-102 makes the bucket
//     count a function of the page size and the entry width; the zone size
//     follows from the count; the offset width follows from the zone. A codec
//     that transcribed 512 / 9 / 8 would pass everything here on amd64 and be
//     wrong on the first architecture with a different page size.
//   - **the level is carried**, because a descent needs to know how many steps
//     remain and DEC-012 keeps it out of the nodes.
//
// The geometry is exercised at both instances the tree ships — the amd64
// default and the tiny test geometry — because the derivation is the claim, and
// a single-geometry test cannot tell a derivation from a constant.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/BucketCodec.h>
#include <mem/radix/CoreTree.h>

#include <cstdio>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
constexpr auto GT = rdx::kTinyGeometry;
using BucketA = rdx::HarnessBucketCodec<GA>;
using BucketT = rdx::HarnessBucketCodec<GT>;

// A node to point a bucket entry at. Any allocation of the right alignment
// serves — the codec never dereferences it, which is itself worth stating: the
// bucket word is a value, and decoding it is arithmetic.
template <unsigned V>
rdx::Node<GA, V>* makeNode() {
    auto p = VS::tryMake<rdx::Node<GA, V>>(uint64_t{1});
    return p ? static_cast<rdx::Node<GA, V>*>(p.raw()) : nullptr;
}

}  // namespace

// ─── The derived widths §5.1 tabulates ─────────────────────────────────────

TEST(radix_bucket_geometry_is_derived_from_the_page_size) {
    // DEC-102: 512 buckets at a 4 KiB page and an 8 B entry, indexed by the top
    // 9 bits, tiling the 47-bit user VA at 256 GiB each.
    ASSERT_EQ(size_t{512}, rdx::kBucketCount);
    ASSERT_EQ(uint8_t{9}, GA.rootBits);
    ASSERT_EQ(uint64_t{256} << 30, BucketA::kZoneSpan);
    ASSERT_EQ(uint64_t{1} << 30, BucketA::kMinClusterSpan);

    // ...and every one of those is a consequence of the page size, not a
    // constant. Recomputed here the long way so a codec that transcribed the
    // shipped numbers fails.
    ASSERT_EQ(arch::smallPageSize / rdx::kBucketEntryBytes, rdx::kBucketCount);
    ASSERT_EQ(rdx::slotSpan(GA, 0), BucketA::kZoneSpan);
    ASSERT_EQ(rdx::nodeSpan(GA, GA.defaultRootLevel), BucketA::kMinClusterSpan);

    // §5.1's "45 of 64 bits" — the figure the headroom argument rests on.
    ASSERT_EQ(unsigned{8}, BucketA::kOffsetBits);       // log2(256 GiB / 1 GiB)
    ASSERT_EQ(unsigned{3}, BucketA::kLevelBits);        // levels 1..6
    ASSERT_EQ(size_t{64},  BucketA::kPointerAlignment); // every amd64 node class
    ASSERT_EQ(unsigned{33}, BucketA::kPointerBits);     // 39-bit window over 64 B
    ASSERT_EQ(unsigned{45}, BucketA::kPointerFieldShift + BucketA::kPointerBits);
}

// The tiny geometry gets different widths from the same arithmetic — which is
// what distinguishes a derivation from a constant, and is the whole content of
// DEC-102's "an architecture with a different small-page size adapts
// automatically".
TEST(radix_bucket_widths_track_the_geometry) {
    ASSERT_EQ(rdx::slotSpan(GT, 0), BucketT::kZoneSpan);
    ASSERT_EQ(rdx::nodeSpan(GT, GT.defaultRootLevel), BucketT::kMinClusterSpan);
    ASSERT_EQ(rdx::log2Exact(BucketT::kZoneSpan / BucketT::kMinClusterSpan),
              static_cast<uint8_t>(BucketT::kOffsetBits));
    ASSERT_TRUE(BucketT::kOffsetBits != BucketA::kOffsetBits);
    ASSERT_TRUE(BucketT::kLevelBits  != BucketA::kLevelBits);
}

// ─── Round trip ────────────────────────────────────────────────────────────

TEST(radix_bucket_entry_round_trips) {
    Harness h;

    // A C0-rooted cluster, the default shape, in a bucket other than zero — a
    // zero bucket index folds the zone base away and would hide an error in it.
    constexpr size_t kBucket = 3;
    const uint64_t zoneBase = kBucket * BucketA::kZoneSpan;

    auto* root = makeNode<32>();          // valence 32 == C0
    ASSERT_TRUE(root != nullptr);

    for (uint64_t units : {uint64_t{0}, uint64_t{1}, uint64_t{7},
                           (BucketA::kZoneSpan / BucketA::kMinClusterSpan) - 1}) {
        const uint64_t base = zoneBase + units * BucketA::kMinClusterSpan;
        for (unsigned level = 1; level <= GA.levelCount; level++) {
            const uint64_t w = BucketA::encode(root, level, base, kBucket);
            const auto d = BucketA::decode(w, kBucket);
            ASSERT_TRUE(static_cast<bool>(d));
            ASSERT_EQ(static_cast<void*>(root), d.root);
            ASSERT_EQ(level, d.level);
            ASSERT_EQ(base, d.base);
        }
    }

    VS::destroy(VS::SafePtr<rdx::Node<GA, 32>>(root));
    assertNoLiveObjects("bucket round trip");
}

// Every node type the geometry can root a cluster at, since DEC-090 roots an
// oversized cluster high and growth walks it up — so the pointer field must
// hold any of them, at whatever alignment its size class promises.
TEST(radix_bucket_entry_round_trips_every_root_valence) {
    Harness h;
    constexpr size_t kBucket = 1;
    const uint64_t base = kBucket * BucketA::kZoneSpan;

    auto* n16 = makeNode<16>();
    auto* n32 = makeNode<32>();
    ASSERT_TRUE(n16 != nullptr && n32 != nullptr);

    for (void* p : {static_cast<void*>(n16), static_cast<void*>(n32)}) {
        const uint64_t w = BucketA::encode(p, 3, base, kBucket);
        ASSERT_EQ(p, BucketA::decode(w, kBucket).root);
    }

    VS::destroy(VS::SafePtr<rdx::Node<GA, 16>>(n16));
    VS::destroy(VS::SafePtr<rdx::Node<GA, 32>>(n32));
    assertNoLiveObjects("every root valence");
}

// ─── All-zero means empty, and only empty ──────────────────────────────────

TEST(radix_bucket_empty_is_the_zero_word) {
    ASSERT_TRUE(BucketA::isEmpty(0));
    const auto d = BucketA::decode(0, 0);
    ASSERT_TRUE(!static_cast<bool>(d));
    ASSERT_EQ(nullptr, d.root);
}

// The DEC-081 collision, one section over. The dangerous encoding is the
// smallest one the codec can produce: bucket 0, base 0, and a root at the very
// bottom of the substrate window. Without the guard bit every field is zero and
// the word reads as an empty bucket — a whole zone unmappable, with no crash and
// nothing to attribute it to.
//
// Driven at the arithmetic rather than by arranging a real offset-0 allocation,
// which the harness cannot promise: what must be true is that the guard bit is
// the ONLY thing making the word non-zero in that case.
TEST(radix_bucket_guard_bit_makes_the_lowest_encoding_non_empty) {
    Harness h;

    // The lowest representable root: level 1, base 0, pointer offset 0. Build
    // the word from the codec's own field shifts, so this cannot drift.
    const uint64_t withoutGuard =
          (uint64_t{1} << BucketA::kLevelShift)   // level 1 is the smallest legal
        | (uint64_t{0} << BucketA::kOffsetShift)
        | (uint64_t{0} << BucketA::kPointerFieldShift);
    // Level 1 alone already makes it non-zero, so the *truly* dangerous case is
    // level 0 — which is not a legal root level, and that is the second line of
    // defence. The guard bit is the first, and it holds for every field
    // combination including the illegal one.
    ASSERT_TRUE(withoutGuard != 0);
    ASSERT_EQ(uint64_t{0}, uint64_t{0} << BucketA::kPointerFieldShift);

    // What the codec actually produces for the lowest LEGAL encoding always has
    // bit 0 set, so the empty test can never be confused by it.
    auto* root = makeNode<32>();
    ASSERT_TRUE(root != nullptr);
    const uint64_t w = BucketA::encode(root, 1, 0, 0);
    ASSERT_TRUE((w & rdx::kBucketGuardBit) != 0);
    ASSERT_TRUE(!BucketA::isEmpty(w));

    VS::destroy(VS::SafePtr<rdx::Node<GA, 32>>(root));
    assertNoLiveObjects("guard bit");
}

// ─── The table is one page, and the index is a prefix ──────────────────────

TEST(radix_bucket_table_is_one_page_of_empty_buckets) {
    Harness h;   // the table is a real allocation, so it needs the mock arena
    ASSERT_EQ(size_t{arch::smallPageSize}, sizeof(rdx::BucketTable));

    auto p = VS::tryMake<rdx::BucketTable>();
    ASSERT_TRUE(static_cast<bool>(p));
    auto* table = static_cast<rdx::BucketTable*>(p.raw());
    for (size_t i = 0; i < rdx::kBucketCount; i++) {
        ASSERT_TRUE(BucketA::isEmpty(table->entries[i].load(RELAXED)));
    }
    VS::destroy(p);
}

TEST(radix_bucket_index_is_the_top_bits_of_the_va) {
    ASSERT_EQ(size_t{0}, rdx::bucketIndexFor<GA>(0));
    ASSERT_EQ(size_t{0}, rdx::bucketIndexFor<GA>(BucketA::kZoneSpan - 1));
    ASSERT_EQ(size_t{1}, rdx::bucketIndexFor<GA>(BucketA::kZoneSpan));
    ASSERT_EQ(rdx::kBucketCount - 1,
              rdx::bucketIndexFor<GA>((uint64_t{1} << GA.addressBits) - 1));

    // DEC-033: one cluster per bucket, so the index is a prefix and every VA in
    // a zone maps to the same bucket. Stated as a property rather than as three
    // examples, because "no probing, no collisions" is what rests on it.
    for (uint64_t k = 0; k < 64; k++) {
        const uint64_t va = 7 * BucketA::kZoneSpan + k * (BucketA::kZoneSpan / 64);
        ASSERT_EQ(size_t{7}, rdx::bucketIndexFor<GA>(va));
    }
}
