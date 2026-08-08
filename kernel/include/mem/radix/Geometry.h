//
// radix-tree — the geometry descriptor (DEC-019 / DEC-093 / DEC-102).
//
// "Tree geometry is a compile-time parameter, not a hard-coded constant."
// Everything dimensional in the tree derives from one `constexpr` descriptor:
// valences, slot spans, sub-range granularities, the shortfall bound, the node
// types, the codec field widths. Nothing anywhere may special-case the shipped
// numbers — that is a constraint on the CODE, not a testing convenience (§12):
//
//   - the exhaustive model check runs on a deliberately tiny geometry, and is
//     only exhaustive because the state space shrinks with it;
//   - DEC-093's sign-off makes per-architecture geometry instances an intended
//     use, and requires that retuning the split cost an edit to this descriptor
//     plus re-derived figures and nothing else;
//   - DEC-102 derives the root bucket count from `arch::smallPageSize /
//     sizeof(BucketEntry)`, so a 16 KiB-page architecture gets 2048 buckets over
//     64 GiB zones automatically.
//
// A mechanism that hard-codes 512 buckets, or 32 slots, or a 4 KiB range unit,
// has violated all three at once and the violation is invisible until someone
// tries to retune.
//
// ─── Level numbering ──────────────────────────────────────────────────────
//
// Levels are numbered in the CONCEPTUAL FULL-DEPTH TREE, root-bucket-ward to
// leaf-ward: the root bucket is level 0, and the non-root levels are 1..N. At
// the amd64 default that is
//
//     level 0  root bucket   9 bits   512 slots    256 GiB per slot
//     level 1  G1            4 bits    16 slots     16 GiB per slot
//     level 2  G0            4 bits    16 slots      1 GiB per slot
//     level 3  C0            5 bits    32 slots     32 MiB per slot   <- default root
//     level 4  C1            5 bits    32 slots      1 MiB per slot
//     level 5  C2            4 bits    16 slots     64 KiB per slot
//     level 6  C3            4 bits    16 slots      4 KiB per slot   <- floor
//
// This numbering is not a presentational choice. §6.8/DEC-053 requires the
// acquisition order's depth measure to be growth-invariant, and a level in the
// conceptual tree is the ONLY measure that is: counting steps from the cluster
// root shifts every node by one when a cluster grows, and counting from the
// root bucket differs from that by exactly one at all times, so it shifts
// identically. Levels are therefore absolute here and derived from a node's
// slot span, never counted during a descent.
//

#ifndef CROCOS_RADIX_GEOMETRY_H
#define CROCOS_RADIX_GEOMETRY_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>

namespace kernel::mm::radix {

    // Upper bound on conceptual-tree depth. Sized for headroom over every
    // plausible architecture rather than for the amd64 default's 6, so the
    // descriptor stays a structural type usable as a template parameter.
    inline constexpr unsigned kMaxLevels = 12;

    // ─── The descriptor ────────────────────────────────────────────────────
    //
    // A structural type, so it can be a non-type template parameter: the whole
    // tree is `CoreTree<kAmd64Geometry>` and the compiler sees every dimension.
    struct GeometryDescriptor {
        // Bits consumed by the root bucket index. DEC-102 derives this from the
        // page size and entry width for the kernel instances; a test geometry
        // sets it directly.
        uint8_t rootBits = 0;

        // Bits per non-root level, index 0 == conceptual level 1.
        uint8_t levelBits[kMaxLevels] = {};
        uint8_t levelCount = 0;

        // log2 of the finest span the tree can represent — the *capability*
        // floor, distinct from placement granularity (DEC-013). 12 == 4 KiB.
        uint8_t floorBits = 0;

        // Total addressable bits the tree tiles. 47 for the amd64 user VA.
        uint8_t addressBits = 0;

        // Conceptual level a cluster's root sits at by default (DEC-031/090).
        // Levels above it exist only for grown or large-rooted clusters.
        uint8_t defaultRootLevel = 0;

        // Bits a slot word has available for the leaf sub-range field
        // (DEC-024's budget: 25 at the amd64 default — bits 63:39 are constant
        // given VMSubstrate at root[510]). The codec's own field widths derive
        // from this; it lives in the geometry because DEC-025's per-level
        // range-unit derivation consumes it.
        uint8_t rangeBudgetBits = 0;
    };

    // ─── Derived quantities ────────────────────────────────────────────────
    //
    // Free functions over a descriptor rather than members of it, so the
    // descriptor stays a plain aggregate (and therefore structural). Every one
    // is consteval-friendly.

    // Sum of the bit widths of levels strictly below `level`, plus the floor:
    // i.e. log2 of one slot's span at `level`.
    constexpr unsigned slotSpanBits(const GeometryDescriptor& g, unsigned level) {
        unsigned bits = g.floorBits;
        for (unsigned l = level + 1; l <= g.levelCount; l++) {
            bits += g.levelBits[l - 1];
        }
        return bits;
    }

    constexpr uint64_t slotSpan(const GeometryDescriptor& g, unsigned level) {
        return uint64_t{1} << slotSpanBits(g, level);
    }

    constexpr unsigned levelIndexBits(const GeometryDescriptor& g, unsigned level) {
        return (level == 0) ? g.rootBits : g.levelBits[level - 1];
    }

    // Slots per node at `level`. Level 0 is the root bucket table, whose "node"
    // is the root page and which carries no state word (§5.1).
    constexpr unsigned valence(const GeometryDescriptor& g, unsigned level) {
        return 1u << levelIndexBits(g, level);
    }

    constexpr unsigned nodeSpanBits(const GeometryDescriptor& g, unsigned level) {
        return slotSpanBits(g, level) + levelIndexBits(g, level);
    }

    constexpr uint64_t nodeSpan(const GeometryDescriptor& g, unsigned level) {
        return uint64_t{1} << nodeSpanBits(g, level);
    }

    // The whole-node claim mask (§5.3). Spelled from the valence, NEVER as ~0:
    // at valence 16 a `~0` mask over-claims 16 bits that do not exist, sets the
    // dying mark, fills the occupancy count with ones, and makes the matching
    // release `fetch_and` CLEAR A DYING MARK — the corrected operand DEC-040's
    // rationale records. The 1ULL is equally load-bearing: `1 << 32` is
    // undefined behaviour on a 32-bit int.
    constexpr uint64_t valenceMask(const GeometryDescriptor& g, unsigned level) {
        return (uint64_t{1} << valence(g, level)) - 1;
    }

    // DEC-025: sub-range granularity is the finest power of two the codec's bit
    // budget allows at each level, clamped at the resolution floor.
    //
    // With an inclusive-end encoding over `units = slotSpan >> unitBits` units,
    // each endpoint needs log2(units) == slotSpanBits - unitBits bits, and both
    // endpoints must fit the budget. So the unit is the smallest (finest) one
    // satisfying 2*(slotSpanBits - unitBits) <= budget, floored.
    //
    // At the amd64 default this reproduces §5.2's table exactly: G1 4 MiB,
    // G0 256 KiB, C0 8 KiB, C1 and below 4 KiB. That it is DERIVED and not
    // transcribed is the point — DEC-039 retuned C0 from 4 KiB to 8 KiB by
    // changing the bit split, and a transcribed table would have silently kept
    // the old value.
    constexpr unsigned rangeUnitBits(const GeometryDescriptor& g, unsigned level) {
        const unsigned span = slotSpanBits(g, level);
        unsigned unit = g.floorBits;
        while (unit < span && 2u * (span - unit) > g.rangeBudgetBits) unit++;
        return unit;
    }

    constexpr unsigned rangeEndpointBits(const GeometryDescriptor& g, unsigned level) {
        return slotSpanBits(g, level) - rangeUnitBits(g, level);
    }

    // Units per slot span at `level` — the number of distinct sub-range
    // positions a leaf at this level can express.
    constexpr uint64_t rangeUnitCount(const GeometryDescriptor& g, unsigned level) {
        return uint64_t{1} << rangeEndpointBits(g, level);
    }

    // §6.1's per-site allocation bound: how many levels a publish site must
    // descend before a floor-granular boundary becomes expressible. 0 at C1 and
    // below, 1 at C0, 2 at G0, 3 at G1 under the amd64 default.
    //
    // Derived by descending until the range unit reaches the floor, rather than
    // by arithmetic on the unit widths — the two agree only when every level's
    // bit width is equal, which is exactly what the shipped split is not.
    constexpr unsigned resolutionShortfall(const GeometryDescriptor& g, unsigned level) {
        unsigned k = 0;
        while (level + k < g.levelCount && rangeUnitBits(g, level + k) > g.floorBits) k++;
        return k;
    }

    // §6.1's site bound: 1 + 2*(L-1), where L is the number of claimable levels
    // at and below the cluster root. The `1` is the topmost node containing the
    // range, which IS the cluster root — reading it as "below the cluster root"
    // gives a smaller number and undersizes the fixed-capacity claim set.
    //
    // Computed for the DEEPEST possible cluster root (level 1, a fully grown
    // cluster), because the claim set is a fixed-capacity object sized once.
    constexpr unsigned siteBound(const GeometryDescriptor& g) {
        const unsigned claimableLevels = g.levelCount;   // levels 1..levelCount
        return 1u + 2u * (claimableLevels - 1u);
    }

    // The VA span one root bucket tiles (§5.1's "zone").
    constexpr uint64_t zoneSpan(const GeometryDescriptor& g) {
        return slotSpan(g, 0);
    }

    // The smallest span a cluster root can have: the default root level's node
    // span (§5.1). The bucket codec's base-offset field is denominated in these.
    constexpr uint64_t minClusterSpan(const GeometryDescriptor& g) {
        return nodeSpan(g, g.defaultRootLevel);
    }

    // ─── Well-formedness ───────────────────────────────────────────────────
    //
    // Checked as a predicate rather than as scattered static_asserts so a
    // candidate geometry can be validated in one place, including at the point
    // a test geometry is declared.
    constexpr bool isWellFormed(const GeometryDescriptor& g) {
        if (g.levelCount == 0 || g.levelCount > kMaxLevels) return false;
        if (g.rootBits == 0) return false;
        if (g.defaultRootLevel == 0 || g.defaultRootLevel > g.levelCount) return false;

        // The split must tile the address space exactly, with no slack (§5.1).
        // Slack would mean either unaddressable VA or a level whose slots
        // overlap, and both are silent.
        unsigned total = g.rootBits + g.floorBits;
        for (unsigned i = 0; i < g.levelCount; i++) {
            if (g.levelBits[i] == 0) return false;
            total += g.levelBits[i];
        }
        if (total != g.addressBits) return false;

        // DEC-039: no non-root level exceeds 32 slots. The binding reason is the
        // INTERLOCK, not the size class — at 32 the claim bitmap, a 0..32
        // occupancy count and the dying mark fit one 64-bit word, so every
        // primitive is a single atomic on one location. At 64 the bitmap alone
        // fills the word and the check-then-act race between insertion and
        // reclamation returns.
        for (unsigned i = 0; i < g.levelCount; i++) {
            if (g.levelBits[i] > 5) return false;
        }

        // Every level's sub-range encoding must fit the codec's budget.
        //
        // Note what this does NOT reject, deliberately: a budget so small that
        // rangeUnitBits collapses to the whole slot span. That geometry is
        // degenerate — a leaf can express only its full slot, so every partial
        // operation subdivides — but it is CORRECT, because "the range encoding
        // is never load-bearing": insufficient resolution costs nodes, never
        // semantics. Rejecting it would also reject the deliberately-starved
        // test geometry that exists to make the shortfall rows reachable.
        for (unsigned l = 1; l <= g.levelCount; l++) {
            if (2u * rangeEndpointBits(g, l) > g.rangeBudgetBits) return false;
        }

        // The deepest level must reach the floor exactly, or the tree cannot
        // represent a page.
        //
        // This is also what makes every resolution shortfall RESOLVABLE by
        // descending, which subdivision depends on: the deepest level's slot
        // span IS the floor, so its range unit is the floor whatever the budget,
        // so `level + resolutionShortfall(level) <= levelCount` holds for every
        // level as a consequence. Checking that separately would be a check that
        // can never fire; the test suite asserts the implication instead.
        if (slotSpanBits(g, g.levelCount) != g.floorBits) return false;

        return true;
    }

    // ─── The amd64 default (§5.1) ──────────────────────────────────────────

    // DEC-102: the bucket count is COMPUTED, never stated. A bucket entry is one
    // 64-bit packed word (DEC-103), so at a 4 KiB page this is 512 buckets and
    // 9 index bits; at 16 KiB it is 2048 and 11, with every derived figure
    // following automatically.
    inline constexpr size_t kBucketEntryBytes = 8;
    inline constexpr size_t kBucketCount = arch::smallPageSize / kBucketEntryBytes;

    constexpr uint8_t log2Exact(uint64_t v) {
        uint8_t n = 0;
        while ((uint64_t{1} << n) < v) n++;
        return n;
    }
    static_assert((uint64_t{1} << log2Exact(kBucketCount)) == kBucketCount,
                  "bucket count must be a power of two — smallPageSize / 8 (DEC-102)");

    // The user-VA width the tree tiles. 47 bits on amd64 (the canonical low
    // half). Not arch::* today because no such constant exists; when one lands
    // this becomes a reference to it, which is a one-line change precisely
    // because everything else derives.
    inline constexpr uint8_t kAmd64UserAddressBits = 47;

    // DEC-024: bits 63:39 are constant given VMSubstrate at root[510] / 512 GiB,
    // leaving 25 upper bits for the leaf sub-range. Derived from the substrate's
    // own window width so a VAS-layout change invalidates one codec instance
    // rather than the design (DEC-023).
    inline constexpr uint8_t kAmd64SlotRangeBudgetBits = 64 - 39;

    inline constexpr GeometryDescriptor kAmd64Geometry = {
        .rootBits    = log2Exact(kBucketCount),          // 9 at a 4 KiB page
        .levelBits   = { 4, 4, 5, 5, 4, 4 },             // G1 G0 C0 C1 C2 C3
        .levelCount  = 6,
        .floorBits   = log2Exact(arch::smallPageSize),   // 12
        .addressBits = kAmd64UserAddressBits,
        .defaultRootLevel = 3,                           // C0 — 1 GiB (DEC-031)
        .rangeBudgetBits  = kAmd64SlotRangeBudgetBits,
    };

    static_assert(isWellFormed(kAmd64Geometry),
                  "the amd64 geometry must tile the user VA exactly, cap valence at 32, "
                  "and fit every level's sub-range in the codec budget");

    // The figures §5.1 and §5.2 tabulate, asserted rather than trusted. If a
    // retune changes them these fire and the tables get re-derived — which is
    // DEC-093's constraint made mechanical.
    static_assert(valence(kAmd64Geometry, 0) == 512);
    static_assert(valence(kAmd64Geometry, 3) == 32 && valence(kAmd64Geometry, 4) == 32);
    static_assert(valence(kAmd64Geometry, 1) == 16 && valence(kAmd64Geometry, 2) == 16);
    static_assert(valence(kAmd64Geometry, 5) == 16 && valence(kAmd64Geometry, 6) == 16);
    static_assert(nodeSpan(kAmd64Geometry, 0) == (uint64_t{1} << 47));
    static_assert(slotSpan(kAmd64Geometry, 0) == (uint64_t{256} << 30));   // 256 GiB zone
    static_assert(nodeSpan(kAmd64Geometry, 3) == (uint64_t{1} << 30));     // 1 GiB cluster
    static_assert(slotSpan(kAmd64Geometry, 6) == arch::smallPageSize);     // 4 KiB floor
    static_assert(minClusterSpan(kAmd64Geometry) == (uint64_t{1} << 30));

    // §5.2's range-unit table, derived.
    static_assert(rangeUnitBits(kAmd64Geometry, 1) == 22);   // G1  4 MiB
    static_assert(rangeUnitBits(kAmd64Geometry, 2) == 18);   // G0  256 KiB
    static_assert(rangeUnitBits(kAmd64Geometry, 3) == 13);   // C0  8 KiB
    static_assert(rangeUnitBits(kAmd64Geometry, 4) == 12);   // C1  4 KiB — floor reached
    static_assert(rangeUnitBits(kAmd64Geometry, 5) == 12);
    static_assert(rangeUnitBits(kAmd64Geometry, 6) == 12);

    // §5.2's shortfall column, and DEC-039's "one bit spare at C0".
    static_assert(resolutionShortfall(kAmd64Geometry, 1) == 3);
    static_assert(resolutionShortfall(kAmd64Geometry, 2) == 2);
    static_assert(resolutionShortfall(kAmd64Geometry, 3) == 1);
    static_assert(resolutionShortfall(kAmd64Geometry, 4) == 0);
    static_assert(2 * rangeEndpointBits(kAmd64Geometry, 3) == 24,
                  "C0's inclusive-end range must cost 24 of DEC-024's 25 bits — "
                  "the one spare bit DEC-081's audit stands on");

    // §6.1: 11 sites fully grown, 7 un-grown.
    static_assert(siteBound(kAmd64Geometry) == 11);

    // ─── Test geometries (§12) ─────────────────────────────────────────────
    //
    // "Instantiating on a tiny geometry — a handful of bits per level over a toy
    // address space — shrinks the reachable state space enough that the model
    // check can be EXHAUSTIVE rather than a statistical sweep hoping to hit the
    // expand-versus-reclaim window."
    //
    // 2 bits root + 2/2/2 levels + 2-bit floor == 10-bit address space: 1 KiB of
    // VA, 4 buckets of 256 B, leaves down to 4 B. Small enough to enumerate.
    inline constexpr GeometryDescriptor kTinyGeometry = {
        .rootBits    = 2,
        .levelBits   = { 2, 2, 2 },
        .levelCount  = 3,
        .floorBits   = 2,
        .addressBits = 10,
        .defaultRootLevel = 2,
        // The full codec budget. The tiny geometry is for exhaustive STRUCTURAL
        // coverage, and a budget that forced a coarse range unit would remove
        // the sub-range shapes from the enumeration entirely — every level here
        // reaches the floor, which is what the static_assert below pins.
        .rangeBudgetBits = kAmd64SlotRangeBudgetBits,
    };
    static_assert(isWellFormed(kTinyGeometry));
    static_assert(valence(kTinyGeometry, 0) == 4);
    static_assert(nodeSpan(kTinyGeometry, 0) == 1024);
    static_assert(slotSpan(kTinyGeometry, 3) == 4);
    static_assert(rangeUnitBits(kTinyGeometry, 1) == 2,
                  "the tiny geometry's budget must reach the floor at every level, or "
                  "the exhaustive check silently loses the sub-range dispatch rows");

    // A second tiny geometry that deliberately DOES have a resolution shortfall,
    // so the shortfall-driven subdivision rows (§6.1, §6.3) are reachable
    // exhaustively. Without this the tiny sweep would cover only the shapes the
    // generous-budget geometry can express, and §6.1's per-level allocation
    // bound — the one an implementer is most likely to flatten to a constant —
    // would never be exercised at all.
    inline constexpr GeometryDescriptor kTinyShortfallGeometry = {
        .rootBits    = 2,
        .levelBits   = { 2, 2, 2 },
        .levelCount  = 3,
        .floorBits   = 2,
        .addressBits = 10,
        .defaultRootLevel = 2,
        .rangeBudgetBits = 5,   // forces a coarse unit at the upper levels
    };
    static_assert(isWellFormed(kTinyShortfallGeometry));
    static_assert(resolutionShortfall(kTinyShortfallGeometry, 1) > 0,
                  "kTinyShortfallGeometry exists to make the shortfall rows reachable");

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_GEOMETRY_H
