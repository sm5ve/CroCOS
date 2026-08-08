//
// radix-tree Phase 1 — the geometry descriptor.
//
// Most of Geometry.h's contract is static_asserted at namespace scope, so this
// file's job is the part a static_assert cannot do: check that the DERIVATIONS
// hold for geometries other than the shipped one. A derivation that happens to
// agree with the amd64 numbers while being wrong in general is exactly the
// failure DEC-093's constexpr-only-retuning constraint is meant to prevent, and
// it would be invisible in a suite that only ever instantiates one geometry.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <mem/radix/Geometry.h>

using namespace CroCOSTest;
namespace rdx = kernel::mm::radix;

namespace {

// A third geometry, unlike both shipped ones: uniform 3-bit levels over a
// 20-bit space with a 2-bit floor. Nothing in the tree may behave differently
// for it.
constexpr rdx::GeometryDescriptor kUniformGeometry = {
    .rootBits    = 3,
    .levelBits   = { 3, 3, 3, 3, 3 },
    .levelCount  = 5,
    .floorBits   = 2,
    .addressBits = 20,
    .defaultRootLevel = 3,
    .rangeBudgetBits  = 24,
};
static_assert(rdx::isWellFormed(kUniformGeometry));

}  // namespace

TEST(radix_geometry_tiles_the_address_space_exactly) {
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyGeometry,
                           rdx::kTinyShortfallGeometry, kUniformGeometry }) {
        // The root node's span is the whole address space, and the deepest
        // level's slot span is the floor. Slack in either direction is silent:
        // unaddressable VA at one end, overlapping slots at the other.
        ASSERT_EQ(uint64_t{1} << g.addressBits, rdx::nodeSpan(g, 0));
        ASSERT_EQ(uint64_t{1} << g.floorBits, rdx::slotSpan(g, g.levelCount));
    }
}

TEST(radix_geometry_slot_spans_nest) {
    // Each level's node span is its parent's slot span. This is what makes the
    // tree a prefix index at all, and what lets a descent derive a subtree's
    // base by masking rather than by storing anything in a node.
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyGeometry, kUniformGeometry }) {
        for (unsigned l = 0; l < g.levelCount; l++) {
            ASSERT_EQ(rdx::slotSpan(g, l), rdx::nodeSpan(g, l + 1));
        }
    }
}

TEST(radix_geometry_valence_never_exceeds_the_interlock_cap) {
    // DEC-039. Not a size-class result: at 32 the claim bitmap, a 0..32 count
    // and the dying mark fit ONE word, so every primitive is a single atomic on
    // one location. At 64 the bitmap alone fills the word and the check-then-act
    // race between insertion and reclamation returns.
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyGeometry, kUniformGeometry }) {
        for (unsigned l = 1; l <= g.levelCount; l++) {
            ASSERT_TRUE(rdx::valence(g, l) <= 32);
            // ...and the whole-node mask must be spelled from the valence.
            const uint64_t mask = rdx::valenceMask(g, l);
            ASSERT_EQ(rdx::valence(g, l), __builtin_popcountll(mask));
            ASSERT_EQ(uint64_t{0}, mask >> rdx::valence(g, l));
        }
    }
}

TEST(radix_geometry_range_units_fit_the_codec_budget) {
    // DEC-025's derivation, checked as the property rather than as the amd64
    // table: every level's inclusive-end encoding must fit the budget, and the
    // unit must be the FINEST that does — one bit finer must not fit, unless we
    // are already at the floor.
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyGeometry,
                           rdx::kTinyShortfallGeometry, kUniformGeometry }) {
        for (unsigned l = 1; l <= g.levelCount; l++) {
            const unsigned unit = rdx::rangeUnitBits(g, l);
            const unsigned span = rdx::slotSpanBits(g, l);
            ASSERT_TRUE(unit >= g.floorBits);
            ASSERT_TRUE(2 * (span - unit) <= g.rangeBudgetBits);
            if (unit > g.floorBits) {
                ASSERT_TRUE(2 * (span - (unit - 1)) > g.rangeBudgetBits);
            }
        }
    }
}

TEST(radix_geometry_shortfall_counts_levels_not_bits) {
    // §6.1's allocation bound is "one node per publish site PER LEVEL of
    // range-resolution shortfall". Counting bits instead of levels agrees only
    // when every level has the same width — which the shipped split is
    // specifically not — so the two are checked to disagree here on purpose.
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyShortfallGeometry }) {
        for (unsigned l = 1; l <= g.levelCount; l++) {
            const unsigned k = rdx::resolutionShortfall(g, l);
            // Descending exactly k levels reaches the floor, and k-1 does not.
            ASSERT_EQ(g.floorBits, rdx::rangeUnitBits(g, l + k));
            if (k > 0) ASSERT_TRUE(rdx::rangeUnitBits(g, l + k - 1) > g.floorBits);
        }
    }
    // The amd64 column, spelled out, because §6.1's bound quotes it directly.
    ASSERT_EQ(3u, rdx::resolutionShortfall(rdx::kAmd64Geometry, 1));
    ASSERT_EQ(2u, rdx::resolutionShortfall(rdx::kAmd64Geometry, 2));
    ASSERT_EQ(1u, rdx::resolutionShortfall(rdx::kAmd64Geometry, 3));
    ASSERT_EQ(0u, rdx::resolutionShortfall(rdx::kAmd64Geometry, 4));
}

TEST(radix_geometry_rejects_malformed_descriptors) {
    // isWellFormed is the single gate a candidate geometry passes through, so
    // its negative cases matter as much as its positive one.
    auto g = rdx::kTinyGeometry;

    auto slack = g; slack.addressBits = 11;                 // does not tile exactly
    ASSERT_FALSE(rdx::isWellFormed(slack));

    auto overValence = g; overValence.levelBits[0] = 6;     // 64 slots — DEC-039
    ASSERT_FALSE(rdx::isWellFormed(overValence));

    auto zeroLevel = g; zeroLevel.levelBits[1] = 0;
    ASSERT_FALSE(rdx::isWellFormed(zeroLevel));

    auto noRoot = g; noRoot.rootBits = 0;
    ASSERT_FALSE(rdx::isWellFormed(noRoot));

    auto badDefault = g; badDefault.defaultRootLevel = 9;
    ASSERT_FALSE(rdx::isWellFormed(badDefault));

    // Note what is NOT here: a geometry whose shortfall cannot be resolved by
    // descending. It is unconstructible — the deepest level's slot span IS the
    // floor, so its range unit is the floor whatever the budget, and every
    // shortfall therefore terminates inside the tree. The implication is
    // asserted below rather than guarded by a check that could never fire.
}

TEST(radix_geometry_every_shortfall_resolves_within_the_tree) {
    // Subdivision recurses until the boundary is expressible. Running past the
    // deepest level would be an unbounded recursion ending at an assert, so this
    // is the property buildSubtree's depth assert rests on.
    for (const auto& g : { rdx::kAmd64Geometry, rdx::kTinyGeometry,
                           rdx::kTinyShortfallGeometry, kUniformGeometry }) {
        for (unsigned l = 1; l <= g.levelCount; l++) {
            ASSERT_TRUE(l + rdx::resolutionShortfall(g, l) <= g.levelCount);
        }
    }
}

// A budget too small to express any partial range collapses every level to a
// single unit. That is degenerate — every partial operation subdivides instead
// of shrinking in place — but it is CORRECT, because "the range encoding is
// never load-bearing": insufficient resolution costs nodes, never semantics.
// Asserted so nobody "fixes" isWellFormed into rejecting it and takes the
// deliberately-starved shortfall test geometry with it.
TEST(radix_geometry_a_starved_budget_is_degenerate_not_malformed) {
    auto g = rdx::kTinyGeometry;
    g.rangeBudgetBits = 1;
    ASSERT_TRUE(rdx::isWellFormed(g));
    for (unsigned l = 1; l < g.levelCount; l++) {
        // One unit per slot: only the full span is expressible.
        ASSERT_EQ(uint64_t{1}, rdx::rangeUnitCount(g, l));
    }
    // ...and every shortfall still resolves within the tree.
    for (unsigned l = 1; l <= g.levelCount; l++) {
        ASSERT_TRUE(l + rdx::resolutionShortfall(g, l) <= g.levelCount);
    }
}

TEST(radix_geometry_bucket_count_is_derived_not_stated) {
    // DEC-102: an architecture with a different small-page size adapts with no
    // edits beyond the arch constants. Asserted as the relationship, so a future
    // hard-coded 512 fails here.
    ASSERT_EQ(arch::smallPageSize / rdx::kBucketEntryBytes, rdx::kBucketCount);
    ASSERT_EQ(rdx::kBucketCount, rdx::valence(rdx::kAmd64Geometry, 0));
    ASSERT_EQ(uint64_t{1} << rdx::kAmd64Geometry.rootBits, rdx::kBucketCount);
}

TEST(radix_geometry_site_bound_counts_the_cluster_root) {
    // §6.1: "The `1` is the topmost node containing the range, which IS the
    // cluster root; reading it as 'below the cluster root' gives 9 and 5 and
    // UNDERSIZES a fixed-capacity claim set." The undersized reading is the bug,
    // so the assertion is against the correct value specifically.
    ASSERT_EQ(11u, rdx::siteBound(rdx::kAmd64Geometry));
    ASSERT_TRUE(rdx::siteBound(rdx::kAmd64Geometry) > 9u);
}
