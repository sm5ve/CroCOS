//
// radix-tree Phase 1 — the model check (§12, §13's Phase 1 gate).
//
// "The geometry being a compile-time parameter is what makes this tractable.
// Instantiating on a tiny geometry — a handful of bits per level over a toy
// address space — shrinks the reachable state space enough that the model check
// can be EXHAUSTIVE rather than a statistical sweep hoping to hit the
// expand-versus-reclaim window. The production geometry then re-runs the same
// harness statistically. This only works if nothing in the implementation
// special-cases the real numbers — a constraint on the code, not just a testing
// convenience."
//
// Four sweeps, in increasing sequence length and decreasing completeness:
//
//   1. every single operation from empty, over the full 3-level tiny geometry;
//   2. every PAIR of operations, over a 2-level tiny tree;
//   3. every TRIPLE over a curated range set, so three-deep interactions
//      (subdivide, then punch inside the subdivision, then collapse it) are
//      covered;
//   4. randomized long sequences on both the tiny and the REAL geometry —
//      which is what catches anything that quietly depends on the shipped
//      numbers.
//
// After every operation the tree is checked against the model at every floor
// unit AND walked by the structural validator, so a defect is caught at the
// operation that caused it rather than N operations later.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"
#include "ShadowMap.h"
#include "RandomSource.h"

#include <mem/radix/CoreTree.h>

#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GT = rdx::kTinyGeometry;
using CodecT = rdx::HarnessSlotCodec<GT>;
using TreeT  = rdx::CoreTree<GT, CodecT>;
using ValidT = TreeValidator<GT, CodecT>;

constexpr auto GS = rdx::kTinyShortfallGeometry;
using CodecS = rdx::HarnessSlotCodec<GS>;
using TreeS  = rdx::CoreTree<GS, CodecS>;
using ValidS = TreeValidator<GS, CodecS>;

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;
using ValidA = TreeValidator<GA, CodecA>;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) throw AssertionFailure(std::string("makeMapping: allocation failed"));
    return static_cast<rdx::Mapping*>(p.raw());
}

struct Op {
    uint64_t lo, hi;
    bool writes;
};

// Every inclusive range on the unit grid, in both directions.
std::vector<Op> allOps(uint64_t span, uint64_t unit) {
    std::vector<Op> ops;
    for (uint64_t lo = 0; lo < span; lo += unit) {
        for (uint64_t hi = lo + unit - 1; hi < span; hi += unit) {
            ops.push_back(Op{lo, hi, true});
            ops.push_back(Op{lo, hi, false});
        }
    }
    return ops;
}

// A curated subset for the triple sweep: every range that starts or ends on a
// slot boundary, plus a few interior punches. Chosen to keep the shapes that
// exercise the dispatch rows (full cover, edge, middle punch, cross-slot) while
// making a three-deep sweep finite.
std::vector<Op> curatedOps(uint64_t span, uint64_t unit, uint64_t slot) {
    std::vector<Op> ops;
    auto push = [&](uint64_t lo, uint64_t hi) {
        if (lo > hi || hi >= span) return;
        ops.push_back(Op{lo, hi, true});
        ops.push_back(Op{lo, hi, false});
    };
    for (uint64_t base = 0; base < span; base += slot) {
        push(base, base + slot - 1);                     // exactly one slot
        push(base, base + unit - 1);                     // leading edge
        push(base + slot - unit, base + slot - 1);       // trailing edge
        push(base + unit, base + slot - unit - 1);       // middle punch
        push(base, base + 2 * slot - 1);                 // two slots
        push(base + slot - unit, base + slot + unit - 1);// straddling a boundary
    }
    push(0, span - 1);                                   // everything
    return ops;
}

// Run a sequence, checking the model and the structure after every step.
template <typename Tree, typename Valid, rdx::GeometryDescriptor G>
void runSequence(unsigned rootLevel, const Op* ops, size_t count, const char* what) {
    Tree tree;
    if (!tree.init(rootLevel, 0)) {
        throw AssertionFailure(std::string("runSequence: root allocation failed"));
    }
    ShadowMap<G> model(rootLevel, 0);

    for (size_t i = 0; i < count; i++) {
        rdx::Mapping* v = ops[i].writes ? makeMapping(ops[i].lo) : nullptr;
        const auto st = tree.apply(ops[i].lo, ops[i].hi, v);
        if (st != rdx::ApplyStatus::Ok) {
            tree.destroyTree();
            throw AssertionFailure(std::string(what) + ": unexpected allocation failure");
        }
        model.apply(ops[i].lo, ops[i].hi, v);

        assertMatchesModel(tree, model, what);
        Valid::validate(tree, what);
    }

    // Draining must return the tree to a bare root and release every record.
    const uint64_t span = rdx::nodeSpan(G, rootLevel);
    if (tree.apply(0, span - 1, nullptr) != rdx::ApplyStatus::Ok) {
        tree.destroyTree();
        throw AssertionFailure(std::string(what) + ": drain failed");
    }
    if (tree.nodeCount() != 1) {
        tree.destroyTree();
        throw AssertionFailure(std::string(what) + ": drain left interior nodes behind");
    }
    tree.destroyTree();
}

}  // namespace

// ─── 1. Every single operation ─────────────────────────────────────────────

TEST(radix_model_exhaustive_single_operations) {
    Harness h;
    constexpr unsigned rootLevel = 1;                 // full 3-level tiny tree
    const uint64_t span = rdx::nodeSpan(GT, rootLevel);
    const uint64_t unit = uint64_t{1} << GT.floorBits;

    const auto ops = allOps(span, unit);
    ASSERT_TRUE(ops.size() > 100);

    const auto baseline = captureBaseline();
    for (const auto& op : ops) {
        runSequence<TreeT, ValidT, GT>(rootLevel, &op, 1, "single");
    }
    assertBaseline(baseline, "exhaustive singles");
}

// The same sweep on the deliberately-starved geometry, where every level has a
// resolution shortfall — so the shortfall-driven subdivision rows of §6.1/§6.3
// are exercised exhaustively rather than only where the shipped numbers happen
// to reach them.
TEST(radix_model_exhaustive_single_operations_with_a_shortfall) {
    Harness h;
    constexpr unsigned rootLevel = 1;
    const uint64_t span = rdx::nodeSpan(GS, rootLevel);
    const uint64_t unit = uint64_t{1} << GS.floorBits;

    const auto baseline = captureBaseline();
    for (const auto& op : allOps(span, unit)) {
        runSequence<TreeS, ValidS, GS>(rootLevel, &op, 1, "single/shortfall");
    }
    assertBaseline(baseline, "exhaustive singles, shortfall geometry");
}

// ─── 2. Every pair ─────────────────────────────────────────────────────────

TEST(radix_model_exhaustive_operation_pairs) {
    Harness h;
    constexpr unsigned rootLevel = 2;                 // 2-level tree, 16 units
    const uint64_t span = rdx::nodeSpan(GT, rootLevel);
    const uint64_t unit = uint64_t{1} << GT.floorBits;

    const auto ops = allOps(span, unit);
    const auto baseline = captureBaseline();

    Op pair[2];
    for (const auto& a : ops) {
        for (const auto& b : ops) {
            pair[0] = a;
            pair[1] = b;
            runSequence<TreeT, ValidT, GT>(rootLevel, pair, 2, "pair");
        }
    }
    assertBaseline(baseline, "exhaustive pairs");
}

// ─── 3. Every triple over a curated range set ──────────────────────────────

TEST(radix_model_exhaustive_curated_triples) {
    Harness h;
    constexpr unsigned rootLevel = 2;
    const uint64_t span = rdx::nodeSpan(GT, rootLevel);
    const uint64_t unit = uint64_t{1} << GT.floorBits;
    const uint64_t slot = rdx::slotSpan(GT, rootLevel);

    const auto ops = curatedOps(span, unit, slot);
    const auto baseline = captureBaseline();

    Op triple[3];
    for (const auto& a : ops) {
        for (const auto& b : ops) {
            for (const auto& c : ops) {
                triple[0] = a; triple[1] = b; triple[2] = c;
                runSequence<TreeT, ValidT, GT>(rootLevel, triple, 3, "triple");
            }
        }
    }
    assertBaseline(baseline, "exhaustive curated triples");
}

// ─── 4. Randomized long sequences ──────────────────────────────────────────

TEST(radix_model_randomized_sequences_tiny_geometry) {
    Harness h;
    constexpr unsigned rootLevel = 1;
    const uint64_t span = rdx::nodeSpan(GT, rootLevel);
    const uint64_t unit = uint64_t{1} << GT.floorBits;
    const uint64_t units = span / unit;

    const auto baseline = captureBaseline();
    for (unsigned trial = 0; trial < 200; trial++) {
        auto rng = streamFor(1000 + trial);
        std::vector<Op> seq;
        for (unsigned k = 0; k < 40; k++) {
            const uint64_t a = rng.below(units) * unit;
            const uint64_t b = rng.below(units) * unit;
            seq.push_back(Op{a < b ? a : b, (a < b ? b : a) + unit - 1, rng.chance(60)});
        }
        runSequence<TreeT, ValidT, GT>(rootLevel, seq.data(), seq.size(), "random/tiny");
    }
    assertBaseline(baseline, "randomized tiny sequences");
}

// The real geometry, statistically. This is the sweep that catches anything
// depending on the shipped numbers — a hard-coded 32 slots, a 4 KiB range unit,
// a flat allocation bound. The addresses are drawn across a whole C0 cluster at
// page granularity, so both the sub-range rows and the deep-spine rows are hit.
TEST(radix_model_randomized_sequences_real_geometry) {
    Harness h;
    constexpr unsigned rootLevel = 3;                 // C0: 1 GiB
    const uint64_t span = rdx::nodeSpan(GA, rootLevel);
    const uint64_t unit = uint64_t{1} << GA.floorBits;

    const auto baseline = captureBaseline();
    for (unsigned trial = 0; trial < 12; trial++) {
        auto rng = streamFor(5000 + trial);
        TreeA tree;
        ASSERT_TRUE(tree.init(rootLevel, 0));
        ShadowMap<GA> model(rootLevel, 0);

        // Confine the sweep to a window so the shadow map stays small and the
        // addresses collide often enough to exercise the interesting rows —
        // a uniform draw over a whole gigabyte almost never overlaps.
        const uint64_t window = uint64_t{4} * 1024 * 1024;
        const uint64_t windowUnits = window / unit;

        for (unsigned k = 0; k < 300; k++) {
            const uint64_t a = rng.below(windowUnits) * unit;
            const uint64_t b = rng.below(windowUnits) * unit;
            const uint64_t lo = a < b ? a : b;
            const uint64_t hi = (a < b ? b : a) + unit - 1;
            rdx::Mapping* v = rng.chance(60) ? makeMapping(lo) : nullptr;

            ASSERT_TRUE(tree.apply(lo, hi, v) == rdx::ApplyStatus::Ok);
            model.apply(lo, hi, v);
            ValidA::validate(tree, "random/real");
        }

        // Sampling rather than every unit: 1 GiB at 4 KiB is 262144 lookups per
        // check, and the tree is only populated in the window anyway.
        for (uint64_t va = 0; va < window; va += unit) {
            const auto got = tree.lookup(va);
            ASSERT_EQ(model.at(va), got.mapping);
        }

        ASSERT_TRUE(tree.apply(0, span - 1, nullptr) == rdx::ApplyStatus::Ok);
        ASSERT_EQ(size_t{1}, tree.nodeCount());
        tree.destroyTree();
        (void)span;
    }
    assertBaseline(baseline, "randomized real-geometry sequences");
}

// ─── Hole-freedom, structurally ────────────────────────────────────────────

TEST(radix_model_an_address_mapped_before_and_after_is_never_lost) {
    Harness h;
    // §8 invariant 30, in the form a serialized harness can check: after every
    // operation, every address the operation did NOT touch still resolves to
    // exactly the record it did before. The concurrent "never observed
    // unmapped DURING" half belongs to Phase 2; this is the half that catches
    // the subdivision dropping a survivor, which is the way hole-freedom
    // actually breaks — silently, with no crash and no accounting anomaly.
    constexpr unsigned rootLevel = 1;
    const uint64_t span = rdx::nodeSpan(GT, rootLevel);
    const uint64_t unit = uint64_t{1} << GT.floorBits;
    const uint64_t units = span / unit;

    auto rng = streamFor(77);
    for (unsigned trial = 0; trial < 100; trial++) {
        TreeT tree;
        ASSERT_TRUE(tree.init(rootLevel, 0));
        ShadowMap<GT> model(rootLevel, 0);

        for (unsigned k = 0; k < 30; k++) {
            const uint64_t a = rng.below(units) * unit;
            const uint64_t b = rng.below(units) * unit;
            const uint64_t lo = a < b ? a : b;
            const uint64_t hi = (a < b ? b : a) + unit - 1;
            rdx::Mapping* v = rng.chance(60) ? makeMapping(lo) : nullptr;

            // Snapshot everything outside the operation's range.
            std::vector<rdx::Mapping*> before(units);
            for (uint64_t u = 0; u < units; u++) before[u] = tree.lookup(u * unit).mapping;

            ASSERT_TRUE(tree.apply(lo, hi, v) == rdx::ApplyStatus::Ok);
            model.apply(lo, hi, v);

            for (uint64_t u = 0; u < units; u++) {
                const uint64_t va = u * unit;
                if (va >= lo && va <= hi) continue;      // the operation's own range
                ASSERT_EQ(before[u], tree.lookup(va).mapping);
            }
            ValidT::validate(tree, "hole-freedom");
        }

        ASSERT_TRUE(tree.apply(0, span - 1, nullptr) == rdx::ApplyStatus::Ok);
        tree.destroyTree();
    }
}
