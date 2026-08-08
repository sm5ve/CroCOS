//
// radix-tree Phase 3 — the validity token and `revalidate` (§3.1, DEC-051/070).
//
// §3.1 asks a lookup to return two things with different lifetimes and warns
// that conflating them is a memory-safety defect:
//
//   - the **counted reference**, which guarantees the RECORD stays alive after
//     the section closes — this is what makes a blocking pager round-trip
//     possible at all, since a reader must close its section before blocking;
//   - the **validity token**, which guarantees NOTHING on its own. "A counted
//     reference does not keep the address mapped."
//
// The second is the counter-intuitive one, so it gets the first test: a
// concurrent partial `munmap` shrinks a leaf's covered range with a single slot
// write that **retires nothing and marks nothing**, so a blocked thread has no
// other signal to observe. If revalidation did not exist, a fault handler that
// resolved `v`, blocked on the pager and came back would install a PTE for an
// address that had been unmapped in between, and nothing in the tree would have
// told it.
//
// The exit gate names one variant specifically — "the full-unmap recycled-node
// variant — the case the pointer-free token exists for" — and it is the reason
// the token carries no node pointer. An earlier shape was
// `(node, slot index, slot word)`; between the section close and the pager's
// reply that node can be reclaimed, graced, destroyed and its slab slot handed
// to an unrelated allocation, and revalidation would then read a live,
// addressable, unrelated object and could compare EQUAL. That test drives
// exactly that sequence and requires the answer to be "no longer valid".
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/CoreTree.h>

#include <cstdio>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;

constexpr uint64_t kPage = 4096;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

}  // namespace

// ─── The undisturbed case ──────────────────────────────────────────────────

TEST(radix_revalidate_holds_when_nothing_changed) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));   // C1 root, 1 MiB slots

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 16 * kPage - 1, m) == rdx::ApplyStatus::Ok);

    auto r = tree.lookup(8 * kPage);
    ASSERT_TRUE(static_cast<bool>(r));
    ASSERT_TRUE(tree.revalidate(r));
    // Idempotent — revalidation takes and drops its own counted reference, so a
    // second call must behave exactly like the first. (A version that leaked or
    // double-released that reference would show up here and nowhere else.)
    ASSERT_TRUE(tree.revalidate(r));

    r = rdx::LookupResult{};
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("revalidate holds");
}

// ─── The case the token exists for: a shrink that signals nothing else ─────

// §3.1: "A concurrent partial `munmap` shrinks a leaf's covered range with a
// single slot write that retires nothing and marks nothing, so no other signal
// exists for a blocked thread to observe."
//
// Note what makes this sharp: the record is STILL ALIVE (the caller holds a
// counted reference and other slots may still name it), the tree is structurally
// unchanged, and the address the caller asked about is simply no longer covered.
// Nothing but the range comparison can see it.
TEST(radix_revalidate_fails_after_a_partial_unmap_shrinks_the_range) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 16 * kPage - 1, m) == rdx::ApplyStatus::Ok);

    // Resolve a VA in the tail of the mapping, then unmap that tail.
    auto r = tree.lookup(12 * kPage);
    ASSERT_TRUE(static_cast<bool>(r));
    ASSERT_EQ(uint64_t{0}, r.lo());
    ASSERT_EQ(16 * kPage - 1, r.hi());
    ASSERT_TRUE(tree.revalidate(r));

    // The shrink. The slot still names `m`; only its covered range moved.
    ASSERT_TRUE(tree.apply(8 * kPage, 16 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    // The record is emphatically still alive — the caller's own reference sees
    // to that — so "is my record still there" is the WRONG question and would
    // answer yes.
    ASSERT_TRUE(r.mapping()->refcountRelaxed() > 0);
    ASSERT_TRUE(!tree.revalidate(r));

    // A VA still inside the survivor revalidates, which is what makes the
    // failure above a range answer rather than a blanket invalidation.
    auto s = tree.lookup(2 * kPage);
    ASSERT_TRUE(static_cast<bool>(s));
    ASSERT_TRUE(tree.revalidate(s));

    r = rdx::LookupResult{};
    s = rdx::LookupResult{};
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("partial unmap shrink");
}

// ─── An overwrite changes identity, not range ──────────────────────────────

TEST(radix_revalidate_fails_when_the_record_is_replaced) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 16 * kPage - 1, m) == rdx::ApplyStatus::Ok);

    auto r = tree.lookup(4 * kPage);
    ASSERT_TRUE(static_cast<bool>(r));

    // MAP_FIXED the identical range with a different record. Same coverage, same
    // leaf, same everything except which record the slot names — so the range
    // half of the token cannot see this and the identity half must.
    auto* n = makeMapping(0);
    ASSERT_TRUE(n != nullptr);
    ASSERT_TRUE(tree.apply(0, 16 * kPage - 1, n) == rdx::ApplyStatus::Ok);

    ASSERT_TRUE(!tree.revalidate(r));

    r = rdx::LookupResult{};
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("record replaced");
}

// ─── The exit gate's named variant: full unmap, recycled node ──────────────

// The case the pointer-free token exists for. The sequence is:
//
//   1. build a subdivided child so the resolved leaf lives in a node BELOW the
//      root — a token holding a node pointer would be holding a pointer to it;
//   2. unmap everything, which empties that node, so §6.4 reclaims it: marked,
//      unlinked, retired;
//   3. let the grace period complete and the node's slab slot go back to the
//      allocator;
//   4. map something new, which allocates fresh nodes — very likely into the
//      recycled slot;
//   5. revalidate.
//
// A `(node, slot, word)` token would here dereference a live, addressable,
// unrelated node — the thing that makes this a memory-safety bug and not merely
// a wrong answer. Re-descending cannot: the descent starts at the root, and the
// root's slot no longer leads anywhere near the old address.
TEST(radix_revalidate_survives_a_full_unmap_that_recycles_the_node) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));   // C1 root, 1 MiB slots

    // Two disjoint sub-ranges in slot 0 force a subdivision.
    auto* a = makeMapping(0);
    auto* b = makeMapping(4 * kPage);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(tree.apply(4 * kPage, 5 * kPage - 1, b) == rdx::ApplyStatus::Ok);
    quiesce(h);
    const size_t nodesWithChild = tree.nodeCount();
    ASSERT_TRUE(nodesWithChild > 1);   // the leaf really is below the root

    auto r = tree.lookup(0);
    ASSERT_TRUE(static_cast<bool>(r));
    ASSERT_TRUE(tree.revalidate(r));

    // Unmap everything. The child empties and is reclaimed.
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(size_t{1}, tree.nodeCount());

    // Now churn allocations so the recycled slab slot is very likely reused —
    // and, crucially, reused by a NODE, which is what makes a stale node pointer
    // point at something live and plausible rather than at poisoned memory.
    std::vector<rdx::Mapping*> keep;
    for (uint64_t k = 0; k < 8; k++) {
        auto* m = makeMapping((16 + 2 * k) * kPage);
        ASSERT_TRUE(m != nullptr);
        keep.push_back(m);
        ASSERT_TRUE(tree.apply((16 + 2 * k) * kPage, (16 + 2 * k) * kPage + kPage - 1, m)
                    == rdx::ApplyStatus::Ok);
    }
    quiesce(h);
    ASSERT_TRUE(tree.nodeCount() > 1);   // fresh nodes were built

    // The address is gone, and revalidation says so.
    ASSERT_TRUE(!tree.revalidate(r));

    r = rdx::LookupResult{};
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("full unmap, recycled node");
}

// ─── The level-relative trap, driven directly ──────────────────────────────

// §3.1 spends a paragraph on why the token records the DECODED value rather than
// the raw slot word: a C0 leaf with range field [0,0] covers 8 KiB, and if a
// 4 KiB `munmap` forces the §6.3 shortfall subdivision, the surviving C1 leaf
// covering 4 KiB has the same `Mapping` pointer bits, the same tag and the same
// [0,0] range field. A **bit-identical word for a halved range**.
//
// This drives that shape at the geometry where it exists — C0, whose range unit
// is 8 KiB (DEC-039's retune) while the floor is 4 KiB — and requires the answer
// to be "no longer valid". An implementation comparing raw words passes every
// other test in this file and fails only here.
TEST(radix_revalidate_sees_a_halved_range_behind_a_bit_identical_word) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(3, 0, h.domain, h.releasePools));   // C0 root: 8 KiB range unit

    // C0's range unit is coarser than the floor, which is the precondition for
    // the trap. Asserted rather than assumed: a retune that made them equal
    // would leave this test silently checking nothing.
    ASSERT_TRUE(rdx::rangeUnitBits(GA, 3) > GA.floorBits);

    // And the premise the whole paragraph rests on, pinned directly: the slot
    // word does NOT encode the level, so the same bits at C0 and at C1 describe
    // ranges of different sizes. If a future codec change embedded the level
    // here, the trap would disappear — and so would this test's meaning, without
    // the test failing. This assertion is what makes that visible.
    {
        auto* probe = makeMapping(0);
        ASSERT_TRUE(probe != nullptr);
        const rdx::SubRange r0{0, 0};
        ASSERT_EQ(CodecA::encodeLeaf(probe, r0, 3), CodecA::encodeLeaf(probe, r0, 4));
        VS::destroy(VS::SafePtr<rdx::Mapping>(probe));
    }

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    // One C0 range unit: 8 KiB, expressible in the C0 leaf's own range field.
    const uint64_t unit = uint64_t{1} << rdx::rangeUnitBits(GA, 3);
    ASSERT_TRUE(tree.apply(0, unit - 1, m) == rdx::ApplyStatus::Ok);

    auto r = tree.lookup(0);
    ASSERT_TRUE(static_cast<bool>(r));
    ASSERT_EQ(uint64_t{0}, r.lo());
    ASSERT_EQ(unit - 1, r.hi());

    // Unmap the upper half. C0 cannot express a 4 KiB boundary, so this forces
    // the shortfall subdivision and the survivor lands one level down.
    ASSERT_TRUE(tree.apply(unit / 2, unit - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);

    // The first half is still mapped to the same record...
    auto s = tree.lookup(0);
    ASSERT_TRUE(static_cast<bool>(s));
    ASSERT_EQ(r.mapping(), s.mapping());
    // ...over HALF the range. This is the comparison a raw-word token cannot make.
    ASSERT_EQ(unit / 2 - 1, s.hi());
    ASSERT_TRUE(!tree.revalidate(r));
    ASSERT_TRUE(tree.revalidate(s));

    r = rdx::LookupResult{};
    s = rdx::LookupResult{};
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("halved range");
}
