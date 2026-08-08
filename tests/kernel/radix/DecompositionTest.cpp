//
// radix-tree §6.5 — decomposed detachment.
//
// §11's decomposition row is one target with three cases, and this file is
// those three:
//
//   1. An over-budget subtree `MAP_FIXED`-replaced, asserting the decomposed
//      result converges to the UNIT PATH's terminal state.
//   2. A range edge landing deep inside a populated child, so a
//      partially-covered child holds an over-budget sub-range and the
//      sub-operation must decompose at its OWN site — including the tail in a
//      sparse sibling ("round-8's orphaned-range shape").
//   3. A decomposed `munmap` over a range containing HOLES, with both a fully-
//      and a partially-intersected empty slot, asserting no `fetch_sub` lands on
//      an empty slot.
//
// ─── Why the comparison is against a second tree ───────────────────────────
//
// §11 words every case as "the mapped set equals the UNIT PATH's". That is a
// relative property, not an absolute one, so the sharpest way to assert it is to
// run the identical operation sequence on two trees that differ ONLY in
// `detachBudget` — one small enough to force decomposition, one large enough
// that the range is always a single unit — and require them to agree at every
// floor unit. A bug that decomposes wrongly shows up as a disagreement; a bug in
// the shared dispatch shows up in neither, which is what the ShadowMap model
// check is for, and both run on both trees.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"
#include "ShadowMap.h"

#include <mem/radix/CoreTree.h>

#include <string>
#include <cstdio>
#include <vector>
#include <map>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GT = rdx::kTinyGeometry;
using CodecT = rdx::HarnessSlotCodec<GT>;
using ValidT = TreeValidator<GT, CodecT>;

// The tiny geometry, rooted at level 1 (level 0 is the conceptual full-tree
// level; `init` requires rootLevel >= 1). Valence 4 everywhere, floor unit 4:
//
//   level 1 span 256, slot 64   ← the cluster root here
//   level 2 span  64, slot 16
//   level 3 span  16, slot  4   (the floor)
//
// Subtree sizes, which are what the budget is measured against: a level-3 node
// alone is 1, and a fully-populated level-2 subtree is 1 + 4 = 5.
//
// So the budget is **1**: a lone level-3 detachment still takes the ordinary
// unit path (1 <= 1), while anything reaching two nodes decomposes. Anything
// larger is too generous to force decomposition at this geometry's scale — at 2
// the three cases below all fit in a single unit and the tests would pass
// without exercising §6.5 at all.
constexpr unsigned kTinyBudget = 1;
constexpr unsigned kRootLevel  = 1;

// The unit-path tree keeps the production budget (64). The whole tiny tree is
// well under it, so it never decomposes — which is the property the comparison
// depends on, asserted in each test rather than assumed.
using TreeSmall = rdx::CoreTree<GT, CodecT, kTinyBudget>;
using TreeUnit  = rdx::CoreTree<GT, CodecT>;

constexpr uint64_t kUnit = uint64_t{1} << GT.floorBits;   // 4

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, 0, baseVA,
                                       rdx::Protection::Read | rdx::Protection::Write,
                                       rdx::Protection::Read | rdx::Protection::Write);
    if (!p) throw AssertionFailure(std::string("makeMapping: allocation failed"));
    return static_cast<rdx::Mapping*>(p.raw());
}

// A pair of trees over the same geometry differing only in detach budget, plus
// the model both must agree with. Every mutation goes through `apply`, so the
// two can never drift by construction of the fixture.
struct Pair {
    TreeSmall decomposed;
    TreeUnit  unitPath;
    // One model per tree. A single model cannot serve both: it stores record
    // POINTERS, and the two trees deliberately hold distinct records for the
    // same logical mapping, so a shared model would report every unit-path
    // lookup as a cover mismatch against the decomposed tree's record.
    ShadowMap<GT> modelSmall;
    ShadowMap<GT> modelUnit;
    // Records are per-tree — two trees cannot name one record, since each takes
    // its own counted references — so identity is carried in a side table:
    // the lockstep pair created by one `apply` shares a token, and comparing
    // tokens is how "the two trees agree at this address" is decided.
    //
    // A token cannot be read back off the record via `offsetFor`, which asserts
    // `faultVA >= baseVA` and would underflow for every address below the token.
    std::map<const rdx::Mapping*, uint64_t> tokenOf;

    Pair(Harness& h, unsigned rootLevel)
        : modelSmall(rootLevel, 0), modelUnit(rootLevel, 0) {
        if (!decomposed.init(rootLevel, 0, h.domain, h.releasePools) ||
            !unitPath.init(rootLevel, 0, h.domain, h.releasePools)) {
            throw AssertionFailure(std::string("Pair: root allocation failed"));
        }
    }

    ~Pair() {
        decomposed.destroyTree();
        unitPath.destroyTree();
    }

    // `token` is 0 for an unmap, otherwise an identity carried into each tree's
    // own record as its baseVA.
    void apply(uint64_t lo, uint64_t hi, uint64_t token) {
        rdx::Mapping* a = nullptr;
        rdx::Mapping* b = nullptr;
        if (token != 0) {
            a = makeMapping(token);
            b = makeMapping(token);
            tokenOf[a] = token;
            tokenOf[b] = token;
        }
        if (decomposed.apply(lo, hi, a) != rdx::ApplyStatus::Ok) {
            throw AssertionFailure(std::string("decomposed tree: apply failed"));
        }
        if (unitPath.apply(lo, hi, b) != rdx::ApplyStatus::Ok) {
            throw AssertionFailure(std::string("unit-path tree: apply failed"));
        }
        modelSmall.apply(lo, hi, a);
        modelUnit.apply(lo, hi, b);
    }

    // The core assertion of §11's row: the decomposed tree's mapped set equals
    // the unit path's, at EVERY floor unit over the whole span.
    void assertConverged(const char* what) const {
        for (uint64_t va = 0; va < rdx::nodeSpan(GT, decomposed.rootLevel()); va += kUnit) {
            const auto d = decomposed.lookup(va);
            const auto u = unitPath.lookup(va);

            const bool dMapped = static_cast<bool>(d);
            const bool uMapped = static_cast<bool>(u);
            if (dMapped != uMapped) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "%s: at VA %llu the decomposed tree says %s but the unit path "
                              "says %s",
                              what, (unsigned long long)va,
                              dMapped ? "mapped" : "unmapped",
                              uMapped ? "mapped" : "unmapped");
                throw AssertionFailure(std::string(msg));
            }
            if (!dMapped) continue;

            // Identity, via the token each tree's record was built with.
            const uint64_t dt = tokenOf.at(d.mapping());
            const uint64_t ut = tokenOf.at(u.mapping());
            if (dt != ut) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "%s: at VA %llu the decomposed tree names token %llu but the "
                              "unit path names token %llu",
                              what, (unsigned long long)va,
                              (unsigned long long)dt, (unsigned long long)ut);
                throw AssertionFailure(std::string(msg));
            }
        }
    }
};

// Fill [lo,hi] one floor unit at a time, which is what actually builds nodes all
// the way to the floor — a single wide apply would write one coarse leaf and
// there would be no subtree to go over budget.
void populateByUnit(Pair& p, uint64_t lo, uint64_t hi, uint64_t tokenBase) {
    for (uint64_t va = lo; va <= hi; va += kUnit) {
        p.apply(va, va + kUnit - 1, tokenBase + va);
    }
}

}  // namespace

// ─── Case 1: an over-budget subtree, MAP_FIXED-replaced ────────────────────

// §11: "On a tiny geometry with a tiny detachBudget, MAP_FIXED-replace a subtree
// larger than the budget ... the terminal state matches the unit path's — a
// single coarse leaf when a value-writing operation fully covers a
// non-cluster-root site, per-slot results otherwise (§6.5)."
TEST(radix_decomposition_over_budget_subtree_converges_to_the_unit_path) {
    Harness h(1, 1);
    Pair p(h, kRootLevel);   // root span 256, four 64 B slots

    // Densely populate [0,63] — the root's slot 0 — to the floor. That builds a
    // level-2 node over four level-3 nodes: 5 nodes behind one root slot, well
    // over the budget of 1.
    populateByUnit(p, 0, 63, 1000);
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 1 populate (decomposed)");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.unitPath, "case 1 populate (unit path)");

    const uint64_t before = p.decomposed.stats().decompositions.load(RELAXED);

    // MAP_FIXED the whole of that subtree's range with one record.
    p.apply(0, 63, 7777);

    // It really did decompose — otherwise this test is asserting nothing about
    // §6.5 at all, and would keep passing if decomposition were deleted.
    ASSERT_TRUE(p.decomposed.stats().decompositions.load(RELAXED) > before);
    ASSERT_EQ(uint64_t{0}, p.unitPath.stats().decompositions.load(RELAXED));

    p.assertConverged("case 1");
    assertMatchesModel(p.decomposed, p.modelSmall, "case 1 decomposed");
    assertMatchesModel(p.unitPath, p.modelUnit, "case 1 unit path");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 1 (decomposed)");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.unitPath, "case 1 (unit path)");

    // The terminal state: the site here is the level-1 node under root slot 0,
    // it is fully covered, it is not the cluster root, and the operation writes
    // a value — so §6.5's final unit applies, and both trees must have collapsed
    // to a SINGLE COARSE LEAF at the root's slot 0.
    //
    // Asserted structurally rather than inferred from lookups: per-slot results
    // and a coarse leaf are indistinguishable by cover, and the whole point of
    // the final unit is that it converges to the coarser shape.
    size_t leavesSmall = 0, nodesSmall = 0;
    p.decomposed.walk([&](unsigned level, uint64_t, rdx::NodeRef node) {
        nodesSmall++;
        for (unsigned i = 0; i < rdx::valence(GT, level); i++) {
            if (CodecT::isLeaf(node.slot(i).load(RELAXED))) leavesSmall++;
        }
    });
    size_t leavesUnit = 0, nodesUnit = 0;
    p.unitPath.walk([&](unsigned level, uint64_t, rdx::NodeRef node) {
        nodesUnit++;
        for (unsigned i = 0; i < rdx::valence(GT, level); i++) {
            if (CodecT::isLeaf(node.slot(i).load(RELAXED))) leavesUnit++;
        }
    });
    ASSERT_EQ(leavesUnit, leavesSmall);
    ASSERT_EQ(nodesUnit, nodesSmall);
    // One leaf, and the only surviving node is the cluster root itself: the
    // level-1 site was flattened and then detached by the final unit.
    ASSERT_EQ(size_t{1}, leavesSmall);
    ASSERT_EQ(size_t{1}, nodesSmall);

    // §11: "the NEW record's count nets to the unit path's — its per-unit
    // increments legitimately exceed the unit path's transiently and converge at
    // the final unit's deleters, so the assertion is on the SETTLED value, not
    // the trajectory."
    quiesce(h);
    auto censusSmall = ValidT::namingSlotCensus(p.decomposed);
    auto censusUnit  = ValidT::namingSlotCensus(p.unitPath);
    ASSERT_EQ(size_t{1}, censusSmall.size());
    ASSERT_EQ(size_t{1}, censusUnit.size());
    ASSERT_EQ(censusUnit.begin()->second, censusSmall.begin()->second);
    ASSERT_EQ(uint64_t{1}, censusSmall.begin()->first->refcountRelaxed());
}

// ─── Case 2: a range edge deep inside a populated child ────────────────────

// §11: "a second case whose range edge lands deep inside a populated child, so a
// partially-covered child holds an over-budget sub-range: assert the
// sub-operation decomposes (the claim-set capacity assert must not fire) and the
// mapped set equals the unit path's over the whole requested range, INCLUDING
// ANY TAIL IN A SPARSE SIBLING (round-8's orphaned-range shape)."
//
// That tail is the whole point. §6.5 records that anchoring the site to the
// "deepest common ancestor of the fully-covered subtrees" rather than to the
// RANGE leaves the tail assigned to no unit at all — a decomposed MAP_FIXED that
// maps only part of its range, silently. This shape is the one that catches it:
// everything dense lives under root slot 0, and the range's tail reaches into
// root slot 1, which is nearly empty.
TEST(radix_decomposition_partial_child_edge_maps_the_tail_in_a_sparse_sibling) {
    Harness h(1, 1);
    Pair p(h, kRootLevel);

    // Dense under root slot 0 ([0,63]) — 5 nodes, over budget.
    populateByUnit(p, 0, 63, 2000);
    // Sparse under root slot 1 ([64,127]): a single mapped unit, sitting PAST
    // where the operation's range ends, so the sibling is genuinely sparse and
    // its untouched part is observable.
    p.apply(104, 107, 3000);

    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 2 populate (decomposed)");

    const uint64_t before = p.decomposed.stats().decompositions.load(RELAXED);

    // The edge lands deep INSIDE the populated child: 16 is not the start of
    // root slot 0, so [16,63] is a PARTIALLY-covered child, and inside it three
    // fully-covered level-3 children put the sub-range over budget — which is
    // the sub-operation that must decompose at its own site. The range then runs
    // on into the sparse sibling, ending at 100.
    p.apply(16, 103, 8888);

    ASSERT_TRUE(p.decomposed.stats().decompositions.load(RELAXED) > before);

    p.assertConverged("case 2");
    assertMatchesModel(p.decomposed, p.modelSmall, "case 2 decomposed");
    assertMatchesModel(p.unitPath, p.modelUnit, "case 2 unit path");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 2 (decomposed)");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.unitPath, "case 2 (unit path)");

    // The orphaned-tail assertion, stated directly rather than left to the
    // sweep: every unit of the tail that lies in the sparse sibling must name
    // the new record. Under the rejected "DCA of the fully-covered subtrees"
    // site predicate these addresses are assigned to no unit and stay as they
    // were — 256..399 unmapped and 400..403 still naming the old record.
    for (uint64_t va = 64; va <= 103; va += kUnit) {
        const auto r = p.decomposed.lookup(va);
        if (!r) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "case 2: tail VA %llu in the sparse sibling was never mapped — "
                          "the site predicate dropped it (§6.5)", (unsigned long long)va);
            throw AssertionFailure(std::string(msg));
        }
        ASSERT_EQ(uint64_t{8888}, p.tokenOf.at(r.mapping()));
    }
    // ...and the part of the sparse sibling PAST the range is untouched.
    {
        const auto r = p.decomposed.lookup(104);
        ASSERT_TRUE(static_cast<bool>(r));
        ASSERT_EQ(uint64_t{3000}, p.tokenOf.at(r.mapping()));
    }
    // ...as is the head of root slot 0, below the range's edge.
    {
        const auto r = p.decomposed.lookup(0);
        ASSERT_TRUE(static_cast<bool>(r));
        ASSERT_EQ(uint64_t{2000}, p.tokenOf.at(r.mapping()));
    }
}

// ─── Case 3: a decomposed munmap over holes ────────────────────────────────

// §11: "a third case: a decomposed munmap over a range containing holes, with
// both a fully- and a partially-intersected empty slot — the shape that reaches
// §6.3's empty-clear row — asserting the mapped set equals the unit path's
// exactly (no hole over- or under-mapped) and that NO fetch_sub lands on any
// empty slot (a MAP_FIXED variant legitimately writes and increments there; only
// the spurious decrement is forbidden, and its borrow is SILENT IN RELEASE)."
//
// The no-fetch_sub half is what §5.3's occupancy range assert exists for: a
// spurious decrement borrows through the count field and, in a debug build,
// trips it. The validator's exact-occupancy check over the quiesced tree is the
// other half — an occupancy that no longer matches the live slot count is
// exactly what a stray decrement leaves behind.
TEST(radix_decomposition_munmap_over_holes_touches_no_empty_slot) {
    Harness h(1, 1);
    Pair p(h, kRootLevel);

    // The level-2 node under root slot 0 has four 16 B slots:
    //   [0,15] populated   [16,31] EMPTY   [32,47] populated   [48,63] EMPTY
    //
    // The munmap below runs [0,51], which makes [16,31] a FULLY-intersected
    // empty slot and [48,63] a PARTIALLY-intersected one — the two shapes §11
    // names, in one operation.
    populateByUnit(p, 0, 15, 4000);
    populateByUnit(p, 32, 47, 5000);

    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 3 populate (decomposed)");
    assertMatchesModel(p.decomposed, p.modelSmall, "case 3 populate");

    const uint64_t before = p.decomposed.stats().decompositions.load(RELAXED);

    p.apply(0, 51, 0);

    ASSERT_TRUE(p.decomposed.stats().decompositions.load(RELAXED) > before);

    p.assertConverged("case 3");
    assertMatchesModel(p.decomposed, p.modelSmall, "case 3 decomposed");
    assertMatchesModel(p.unitPath, p.modelUnit, "case 3 unit path");

    // The occupancy check. `validate` walks the quiesced tree and requires each
    // node's recorded occupancy to equal its live slot count, which a spurious
    // fetch_sub on an empty slot breaks — and which is otherwise invisible,
    // since cover is unaffected by a wrong count until reclamation reads it.
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "case 3 (decomposed)");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.unitPath, "case 3 (unit path)");

    // Everything in the range is gone, and the tail of the partially-intersected
    // empty slot was empty before and is empty after — never "cleared" into
    // something, and never counted.
    for (uint64_t va = 0; va <= 63; va += kUnit) {
        ASSERT_TRUE(!p.decomposed.lookup(va));
    }
}

// ─── The MAP_FIXED variant §11 names in passing ────────────────────────────

// "(a MAP_FIXED variant legitimately writes and increments there; only the
// spurious decrement is forbidden)". Same hole shape, a map instead of an unmap:
// the empty slots MUST be written, so this is the polarity check that stops the
// fix for case 3 being "skip every empty slot".
TEST(radix_decomposition_map_fixed_over_holes_fills_them) {
    Harness h(1, 1);
    Pair p(h, kRootLevel);

    // Same hole shape as case 3.
    populateByUnit(p, 0, 15, 6000);
    populateByUnit(p, 32, 47, 6500);

    const uint64_t before = p.decomposed.stats().decompositions.load(RELAXED);
    p.apply(0, 51, 9999);
    ASSERT_TRUE(p.decomposed.stats().decompositions.load(RELAXED) > before);

    p.assertConverged("map over holes");
    assertMatchesModel(p.decomposed, p.modelSmall, "map over holes decomposed");
    assertMatchesModel(p.unitPath, p.modelUnit, "map over holes unit path");
    // §7.1: displaced `Mapping` references are released by a DeferredRelease
    // record at grace-period end, so between the operation and its grace period
    // the structural counts legitimately exceed the naming-slot census.
    // `validate` checks that census, so it needs a quiesced tree.
    quiesce(h);
    ValidT::validate(p.decomposed, "map over holes (decomposed)");

    // The holes are now mapped — the empty rows were WRITTEN, not skipped —
    // including the partially-intersected one, up to the range's end and no
    // further.
    for (uint64_t va = 0; va <= 51; va += kUnit) {
        const auto r = p.decomposed.lookup(va);
        if (!r) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "map over holes: VA %llu in the hole was not filled — the empty "
                          "row was skipped rather than written (§6.5)",
                          (unsigned long long)va);
            throw AssertionFailure(std::string(msg));
        }
    }
    // ...and the rest of the partially-intersected empty slot is still empty:
    // "writing the full span would map addresses the caller never asked for".
    for (uint64_t va = 52; va <= 63; va += kUnit) {
        ASSERT_TRUE(!p.decomposed.lookup(va));
    }
}
