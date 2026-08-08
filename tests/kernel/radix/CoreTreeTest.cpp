//
// radix-tree Phase 1 — the core tree: descent, subdivision, and the structural
// halves of the §7 lifetime rules.
//
// The two Phase-1 lifetime targets from §13, spelled out:
//
//   - **Mapping count vs naming slots across every dispatch shape**, with no
//     lookup references outstanding. Both directions: a leak and a
//     double-release are opposite failures of one rule, and a check that only
//     detects "too high" passes a premature free.
//   - **The destructor releases nothing.**
//
// Plus the one §11 target that only exists because DEC-067 exists: an operation
// aborted after building its child must have moved no count. That is invisible
// without a mid-operation abort, which is why Phase 0 built the injector.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"
#include "ShadowMap.h"
#include "RandomSource.h"

#include <mem/radix/CoreTree.h>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;
using ValidA = TreeValidator<GA, CodecA>;

constexpr auto GT = rdx::kTinyGeometry;
using CodecT = rdx::HarnessSlotCodec<GT>;
using TreeT  = rdx::CoreTree<GT, CodecT>;
using ValidT = TreeValidator<GT, CodecT>;

// A Mapping in the arena. VMObject is opaque here — the tree's business ends at
// naming the mapping — so a distinct non-null token per record is enough to
// check that translation reaches the right object.
rdx::Mapping* makeMapping(uint64_t baseVA, uint64_t objectOffset = 0,
                          kernel::mm::VMObject* obj = nullptr) {
    auto p = VS::tryMake<rdx::Mapping>(obj, objectOffset, baseVA,
                                       rdx::Protection::Read | rdx::Protection::Write,
                                       rdx::Protection::Read | rdx::Protection::Write |
                                           rdx::Protection::Execute);
    if (!p) throw AssertionFailure(std::string("makeMapping: allocation failed"));
    return static_cast<rdx::Mapping*>(p.raw());
}

void destroyMapping(rdx::Mapping* m) {
    VS::destroy(VS::SafePtr<rdx::Mapping>(m));
}

// RAII over a tree rooted at `level`, so a thrown assertion still tears it down
// and the next test's accounting stays meaningful.
template <typename Tree>
struct TreeFixture {
    Tree tree;
    explicit TreeFixture(Harness& h, unsigned level, uint64_t base = 0) {
        if (!tree.init(level, base, h.domain, h.releasePools)) {
            throw AssertionFailure(std::string("TreeFixture: root allocation failed"));
        }
    }
    // Drain before teardown: retired nodes are destroyed by their DELETERS at
    // grace-period end, so a test that tears down without draining would leave
    // them alive and every accounting assertion would be measuring limbo rather
    // than leaks.
    ~TreeFixture() {
        tree.destroyTree();
    }
    Tree* operator->() { return &tree; }
};

constexpr uint64_t kPage = 4096;

}  // namespace

// ─── Descent (§5.5, DEC-012) ───────────────────────────────────────────────

TEST(radix_tree_lookup_finds_a_leaf_at_the_root_level) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);                 // C0-rooted, 1 GiB, 32 MiB slots
    auto* m = makeMapping(0);

    // A full-slot placement terminates AT the root: leaves live at any level, so
    // this costs one step, not a walk to the floor. That is DEC-003's whole
    // point — a sparse region costs leaves proportional to its aligned-block
    // decomposition, not to its page count.
    const uint64_t slot = rdx::slotSpan(GA, 3);
    ASSERT_TRUE(f->apply(0, slot - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(size_t{1}, f->nodeCount());    // just the root

    ASSERT_EQ(m, f->lookup(0).mapping());
    ASSERT_EQ(m, f->lookup(slot - 1).mapping());
    ASSERT_EQ(m, f->lookup(slot / 2).mapping());
    ASSERT_FALSE(static_cast<bool>(f->lookup(slot)));

    ValidA::validate(f.tree, "root-level leaf");
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());
}

TEST(radix_tree_lookup_walks_to_the_floor_when_the_range_demands_it) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* m = makeMapping(0);

    // One page at the very start. C0's range unit is 8 KiB, so a 4 KiB boundary
    // is inexpressible there and the placement builds a spine down to the floor.
    ASSERT_TRUE(f->apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(m, f->lookup(0).mapping());
    ASSERT_EQ(m, f->lookup(kPage - 1).mapping());
    ASSERT_FALSE(static_cast<bool>(f->lookup(kPage)));

    // Exactly TWO nodes: the C0 root plus one C1 child. §6.1's bound is one node
    // per publish site PER LEVEL OF SHORTFALL, and C0's shortfall is 1 — C1's
    // range unit is already the 4 KiB floor, so the page is expressed as a
    // SUB-RANGE leaf in a 1 MiB C1 slot rather than by building a spine to the
    // bottom. That is precisely what DEC-022's sub-range encoding buys, and a
    // "walk to the floor" implementation would build four nodes here and pass
    // every coverage test while costing double the memory on the commonest
    // shape in the system.
    ASSERT_EQ(size_t{2}, f->nodeCount());
    ValidA::validate(f.tree, "shortfall spine");
}

TEST(radix_tree_lookup_of_an_unmapped_va_is_cheap_and_non_faulting) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    // Nothing mapped: every lookup answers unmapped without dereferencing a
    // Mapping, because `covers` resolves the negative answer from the slot word.
    for (uint64_t va = 0; va < rdx::nodeSpan(GA, 3); va += rdx::slotSpan(GA, 3)) {
        ASSERT_FALSE(static_cast<bool>(f->lookup(va)));
    }
    ValidA::validate(f.tree, "empty tree");
}

TEST(radix_tree_translation_uses_both_terms) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);

    // §5.4: `objectOffset + (faultVA - baseVA)`. Both terms matter, and both
    // slips are silent wrong-frame bugs invisible to the partition invariant and
    // to a shadow interval map — which model COVER, not TRANSLATION. So this
    // gets its own test, as §5.4 instructs.
    const uint64_t base = rdx::slotSpan(GA, 3) * 2;   // not zero, so a missing
                                                     // baseVA term is visible
    const uint64_t objOff = 0x9000;                  // not zero, so a missing
                                                     // objectOffset is visible
    auto* m = makeMapping(base, objOff);
    ASSERT_TRUE(f->apply(base, base + rdx::slotSpan(GA, 3) - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);

    {
        auto r = f->lookup(base + 3 * kPage);
        ASSERT_EQ(m, r.mapping());
        ASSERT_EQ(objOff + 3 * kPage, r.mapping()->offsetFor(base + 3 * kPage));
        // The first byte of the mapping resolves to the object offset itself, not
        // to the object's first byte and not to the leaf's slot base.
        ASSERT_EQ(objOff, r.mapping()->offsetFor(base));
    }
    // Scoped deliberately: a lookup result holds a COUNTED reference, and §11
    // requires the naming-slot census to run with none outstanding — "which
    // would otherwise mask it entirely". Holding `r` here inflates the record's
    // count by one and the validator correctly reports it.
    ValidA::validate(f.tree, "translation");
}

// ─── Subdivision shapes (§6.3, DEC-066) ────────────────────────────────────

TEST(radix_tree_middle_punch_builds_a_spine_not_a_leaf_pair) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* m = makeMapping(0);

    // Map a whole 32 MiB C0 slot, then punch one page out of the middle. §6.3:
    // "a 4 KiB punch through a 32 MiB C0 leaf builds a C1->C2->C3 spine with ~45
    // leaves naming the old record, not two."
    const uint64_t slot = rdx::slotSpan(GA, 3);
    ASSERT_TRUE(f->apply(0, slot - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());

    const uint64_t punch = slot / 2;
    ASSERT_TRUE(f->apply(punch, punch + kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);

    // Coverage is exactly right...
    ASSERT_EQ(m, f->lookup(0).mapping());
    ASSERT_EQ(m, f->lookup(punch - 1).mapping());
    ASSERT_FALSE(static_cast<bool>(f->lookup(punch)));
    ASSERT_FALSE(static_cast<bool>(f->lookup(punch + kPage - 1)));
    ASSERT_EQ(m, f->lookup(punch + kPage).mapping());
    ASSERT_EQ(m, f->lookup(slot - 1).mapping());

    // ...and the shape is a spine of many leaves, not two.
    const auto census = ValidA::namingSlotCensus(f.tree);
    ASSERT_TRUE(census.at(m) > 30);
    // §7.2's middle-punch row: +(k-1), applied literally as +k new naming slots
    // and -1 for the slot that stopped. The count must equal the census.
    ASSERT_EQ(census.at(m), m->refcountRelaxed());
    ValidA::validate(f.tree, "middle punch");
}

// §7.2's edge-subdivision row says the old record's delta is **0**. That figure
// is the MINIMAL SHAPE — the survivor fitting in one child slot — exactly as
// "+1" was the minimal shape for the middle-punch row before DEC-066 generalised
// it. The normative statement is the per-transition rule (+1 per new naming
// slot, -1 for the slot that stopped), and both tests below apply it literally.
//
// The distinction matters because the wide case is the COMMON one: a full-span
// leaf edge-punched by a page leaves a survivor spanning almost the whole node.
// An implementation that hard-codes the table's 0 under-counts by 14 here and
// frees the record while fourteen slots still name it.

TEST(radix_tree_edge_mmap_minimal_shape_leaves_the_old_record_at_delta_zero) {
    Harness h;
    TreeFixture<TreeA> f(h, 5);                 // C2-rooted: 64 KiB slots, 4 KiB units
    auto* oldM = makeMapping(0);
    auto* newM = makeMapping(0);

    // Old record covers exactly two pages, so its survivor is exactly ONE child
    // slot — the shape §7.2's "0" is written for.
    ASSERT_TRUE(f->apply(0, 2 * kPage - 1, oldM) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{1}, oldM->refcountRelaxed());

    ASSERT_TRUE(f->apply(0, kPage - 1, newM) == rdx::ApplyStatus::Ok);

    quiesce(h);

    ASSERT_EQ(newM, f->lookup(0).mapping());
    ASSERT_EQ(oldM, f->lookup(kPage).mapping());
    ASSERT_FALSE(static_cast<bool>(f->lookup(2 * kPage)));

    ASSERT_EQ(uint64_t{1}, oldM->refcountRelaxed());   // +1 new slot, -1 displaced
    ASSERT_EQ(uint64_t{1}, newM->refcountRelaxed());
    ValidA::validate(f.tree, "edge mmap, minimal shape");
}

TEST(radix_tree_edge_mmap_wide_survivor_takes_the_per_transition_delta) {
    Harness h;
    TreeFixture<TreeA> f(h, 5);
    auto* oldM = makeMapping(0);
    auto* newM = makeMapping(0);
    const uint64_t slot = rdx::slotSpan(GA, 5);        // 64 KiB
    const uint64_t children = rdx::valence(GA, 6);     // 16 C3 slots of 4 KiB

    ASSERT_TRUE(f->apply(0, slot - 1, oldM) == rdx::ApplyStatus::Ok);

    quiesce(h);
    ASSERT_EQ(uint64_t{1}, oldM->refcountRelaxed());

    // One page at the edge. The survivor spans the other fifteen child slots, so
    // k = 15 and the delta is +15 - 1 = +14 — NOT the table's 0.
    ASSERT_TRUE(f->apply(0, kPage - 1, newM) == rdx::ApplyStatus::Ok);
    quiesce(h);

    const auto census = ValidA::namingSlotCensus(f.tree);
    ASSERT_EQ(children - 1, census.at(oldM));
    ASSERT_EQ(children - 1, oldM->refcountRelaxed());
    ASSERT_EQ(size_t{1}, census.at(newM));
    ASSERT_EQ(uint64_t{1}, newM->refcountRelaxed());

    // Coverage is what matters, and it is exact.
    ASSERT_EQ(newM, f->lookup(0).mapping());
    for (uint64_t va = kPage; va < slot; va += kPage) {
        ASSERT_EQ(oldM, f->lookup(va).mapping());
    }
    ValidA::validate(f.tree, "edge mmap, wide survivor");
}

TEST(radix_tree_initial_placement_into_k_slots_takes_plus_k) {
    Harness h;
    TreeFixture<TreeA> f(h, 4);                 // C1-rooted: 32 slots of 1 MiB
    auto* m = makeMapping(0);

    // §7.2's first row — the one an event enumeration has no entry for at all,
    // so the record keeps whatever its constructor gave it, and the natural `1`
    // is verbatim the premature free DEC-048 exists to prevent.
    const uint64_t slot = rdx::slotSpan(GA, 4);
    ASSERT_TRUE(f->apply(0, 3 * slot - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);

    ASSERT_EQ(uint64_t{3}, m->refcountRelaxed());
    ASSERT_EQ(size_t{1}, f->nodeCount());    // three sibling leaves, no depth
    ValidA::validate(f.tree, "initial placement +k");
}

TEST(radix_tree_in_place_shrink_moves_no_count) {
    Harness h;
    TreeFixture<TreeA> f(h, 5);                 // C2: 4 KiB units, shortfall 0
    auto* m = makeMapping(0);
    const uint64_t slot = rdx::slotSpan(GA, 5);

    ASSERT_TRUE(f->apply(0, slot - 1, m) == rdx::ApplyStatus::Ok);

    quiesce(h);
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());
    ASSERT_EQ(size_t{1}, f->nodeCount());

    // Edge-partial munmap: one survivor, expressible. No allocation, no
    // subdivision, and — the row most likely to be lost to a shared helper —
    // NO reference-count change.
    ASSERT_TRUE(f->apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());
    ASSERT_EQ(size_t{1}, f->nodeCount());    // still no depth built
    ASSERT_FALSE(static_cast<bool>(f->lookup(0)));
    ASSERT_EQ(m, f->lookup(kPage).mapping());
    ValidA::validate(f.tree, "in-place shrink");
}

// ─── Reclamation (§6.4) ────────────────────────────────────────────────────

TEST(radix_tree_reclamation_actually_reclaims) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* m = makeMapping(0);

    // §11: "Single-threaded map-then-unmap of one leaf must leave the containing
    // node unlinked and retired, asserted DIRECTLY rather than inferred from a
    // memory figure." The historical failure here was deterministic and
    // invisible to every structural check.
    ASSERT_TRUE(f->apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(size_t{2}, f->nodeCount());
    ASSERT_EQ(size_t{1}, liveCountOf<rdx::Mapping>());

    ASSERT_TRUE(f->apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    quiesce(h);
    ASSERT_EQ(size_t{1}, f->nodeCount());    // child gone, cluster root remains

    // The record's last naming slot released, so the COUNTING MECHANISM
    // destroyed it (§7.1: "it, not the RCU deleter, calls destroy<T>, and only
    // at zero"). Asserted through the accounting rather than by reading the
    // record's own count — that read would be a use-after-destroy, which is
    // exactly what the Phase 0 oracle now reports.
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::Mapping>());

    // The cluster root is never reclaimed — its parent slot is a bucket word
    // with no state word to acquire the interlock on, and reclaiming it anyway
    // would leave that word naming freed memory: an entire zone permanently
    // unmappable, with no crash.
    ASSERT_TRUE(f->valid());
    ValidA::validate(f.tree, "after reclamation");
}

TEST(radix_tree_reclamation_stops_at_the_first_non_empty_ancestor) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* keep = makeMapping(0);
    auto* go   = makeMapping(0);

    // Two pages in the same C3 node. Unmapping one must reclaim nothing.
    ASSERT_TRUE(f->apply(0, kPage - 1, go) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_TRUE(f->apply(kPage, 2 * kPage - 1, keep) == rdx::ApplyStatus::Ok);
    const size_t before = f->nodeCount();

    ASSERT_TRUE(f->apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    quiesce(h);
    ASSERT_EQ(before, f->nodeCount());
    ASSERT_EQ(keep, f->lookup(kPage).mapping());
    ValidA::validate(f.tree, "partial reclamation");

    ASSERT_TRUE(f->apply(kPage, 2 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    quiesce(h);
    ASSERT_EQ(size_t{1}, f->nodeCount());
    ValidA::validate(f.tree, "full reclamation");
    // Both records reached zero naming slots and were destroyed by the tree.
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::Mapping>());
}

// ─── Detachment (§6.3's fully-covered-child row) ───────────────────────────

TEST(radix_tree_detaching_a_subtree_releases_every_leaf_reference) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* fine = makeMapping(0);
    auto* coarse = makeMapping(0);

    // Build a populated subtree under one C0 slot: many pages, so many leaves.
    for (uint64_t k = 0; k < 8; k++) {
        ASSERT_TRUE(f->apply(k * 2 * kPage, k * 2 * kPage + kPage - 1, fine)
                    == rdx::ApplyStatus::Ok);
        quiesce(h);
    }
    const uint64_t nameCount = fine->refcountRelaxed();
    ASSERT_TRUE(nameCount >= 8);
    ASSERT_TRUE(f->nodeCount() > 1);

    // MAP_FIXED over the whole C0 slot: the fully-covered-child row detaches the
    // subtree wholesale. "A subtree detach is equivalent to clearing every slot
    // and unlinking every node, executed as one parent-slot store" — so every
    // displaced value follows the rules an individual clear already has.
    const uint64_t slot = rdx::slotSpan(GA, 3);
    ASSERT_TRUE(f->apply(0, slot - 1, coarse) == rdx::ApplyStatus::Ok);
    quiesce(h);

    // Every naming slot released, so `fine` is gone; only `coarse` survives.
    ASSERT_EQ(size_t{1}, liveCountOf<rdx::Mapping>());
    ASSERT_EQ(uint64_t{1}, coarse->refcountRelaxed());
    ASSERT_EQ(size_t{1}, f->nodeCount());              // subtree gone
    ASSERT_EQ(coarse, f->lookup(0).mapping());
    ASSERT_EQ(coarse, f->lookup(slot - 1).mapping());
    ValidA::validate(f.tree, "detach");
    (void)nameCount;
}

// ─── The two Phase-1 lifetime targets ──────────────────────────────────────

TEST(radix_tree_a_mapping_outlives_its_last_naming_slot_and_no_longer) {
    Harness h;
    TreeFixture<TreeA> f(h, 4);                 // C1: 1 MiB slots
    const uint64_t slot = rdx::slotSpan(GA, 4);
    auto* m = makeMapping(0);

    // §11: "Map a region spanning several leaf slots, munmap them one at a time,
    // assert the record survives to the last and dies at it." Run with NO
    // outstanding lookup reference, which would otherwise hold the count above
    // its structural value and mask the whole thing.
    ASSERT_TRUE(f->apply(0, 4 * slot - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{4}, m->refcountRelaxed());

    for (uint64_t k = 0; k < 4; k++) {
        ASSERT_TRUE(f->apply(k * slot, (k + 1) * slot - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        ValidA::validate(f.tree, "incremental unmap");
        if (k < 3) {
            // Survives while any naming slot remains.
            ASSERT_EQ(uint64_t{3 - k}, m->refcountRelaxed());
            ASSERT_EQ(size_t{1}, liveCountOf<rdx::Mapping>());
        }
    }
    // ...and dies AT the last one, not before and not after. Read through the
    // accounting: `m` no longer exists, and touching it would be the
    // use-after-destroy the oracle exists to report.
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::Mapping>());
}

TEST(radix_tree_destructor_releases_nothing) {
    Harness h;
    // Invariant 24: "The destructor releases nothing at all, and neither it nor
    // the deleter ever recurses." The distinction is not cosmetic — a node
    // retired while a foreign CPU's descent cache pins it is destroyed only when
    // that cache turns over, which §7.4 says may never happen, so putting the
    // Mapping release in the destructor would pin every Mapping that node named,
    // and transitively their VMObjects and frames, behind a bound stated in
    // NODES.
    auto* m = makeMapping(0);
    m->acquireRef();
    m->acquireRef();
    ASSERT_EQ(uint64_t{2}, m->refcountRelaxed());

    {
        // A node holding leaf words that name `m`, destroyed directly.
        auto node = VS::tryMake<rdx::Node<GA, 32>>(uint64_t{0});
        ASSERT_TRUE(static_cast<bool>(node));
        for (unsigned i = 0; i < 4; i++) {
            node->slots[i].store(CodecA::encodeLeaf(m, CodecA::fullSpan(4), 4), RELAXED);
        }
        VS::destroy(node);
    }

    // Untouched. If the count had dropped, the destructor was releasing.
    ASSERT_EQ(uint64_t{2}, m->refcountRelaxed());
    (void)m->releaseRef();
    (void)m->releaseRef();
    destroyMapping(m);
}

// ─── DEC-067: an abort moves no count ──────────────────────────────────────

TEST(radix_tree_an_aborted_subdivision_has_moved_no_count) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* m = makeMapping(0);
    auto* newM = makeMapping(0);

    const uint64_t slot = rdx::slotSpan(GA, 3);
    ASSERT_TRUE(f->apply(0, slot - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());

    const auto baseline = captureBaseline();
    const size_t nodesBefore = f->nodeCount();

    // §11 / DEC-067: "an operation aborted after building its child has moved no
    // count". The natural implementation — take the reference while building the
    // subdivision child during acquisition — leaks on EVERY abort path: the
    // discarded child is the only record of the increment, nothing undoes it,
    // and the inflated count pins the record, its VMObject and its frames for
    // the address space's life. Invisible without a mid-operation abort.
    //
    // Sweep the failure index so every depth of the spine gets to be the one
    // that fails.
    for (size_t failAt = 0; failAt < 6; failAt++) {
        injectFailureAt(failAt);
        const auto st = f->apply(kPage, 2 * kPage - 1, newM);
        resetInjection();

        if (st == rdx::ApplyStatus::OutOfMemory) {
            // Nothing published, nothing counted, nothing leaked.
            ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());
            ASSERT_EQ(uint64_t{0}, newM->refcountRelaxed());
            ASSERT_EQ(nodesBefore, f->nodeCount());
            assertBaseline(baseline, "aborted subdivision");
            ValidA::validate(f.tree, "after abort");
        }
    }

    // ...and the same operation succeeds once allocation does.
    ASSERT_TRUE(f->apply(kPage, 2 * kPage - 1, newM) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{1}, newM->refcountRelaxed());
    ValidA::validate(f.tree, "after successful retry");
}

TEST(radix_tree_allocation_failure_is_reported_not_panicked) {
    Harness h;
    TreeFixture<TreeA> f(h, 3);
    auto* m = makeMapping(0);

    // DEC-075 / §10's inverted hazard: every allocation site handles null, and a
    // site written against never-null `make<T>` would re-import the
    // userspace-triggerable panic through the back door.
    injectAllFailures();
    ASSERT_TRUE(f->apply(0, kPage - 1, m) == rdx::ApplyStatus::OutOfMemory);
    quiesce(h);
    resetInjection();

    ASSERT_EQ(uint64_t{0}, m->refcountRelaxed());   // never placed, so still ours
    ASSERT_EQ(size_t{1}, f->nodeCount());
    ValidA::validate(f.tree, "after ENOMEM");
    destroyMapping(m);
}

// ─── Memory returns to baseline ────────────────────────────────────────────

TEST(radix_tree_churn_returns_every_node_and_record) {
    Harness h;
    auto rng = streamFor(11);

    const auto baseline = captureBaseline();
    {
        TreeFixture<TreeT> f(h, 1);
        ShadowMap<GT> model(1, 0);
        const uint64_t span = rdx::nodeSpan(GT, 1);
        const uint64_t unit = uint64_t{1} << GT.floorBits;

        // A FRESH record per placement, which is what the VMM does: every mmap
        // mints a record and hands ownership to the tree, and the tree destroys
        // it when its last naming slot releases (§7.1 — "it, not the RCU
        // deleter, calls destroy<T>, and only at zero").
        //
        // Reusing a fixed pool of records here would be a use-after-destroy the
        // moment one was fully unmapped and then placed again — which is exactly
        // what the Phase 0 oracle reported when this test was first written that
        // way, and exactly the class of defect §10 says the harness is otherwise
        // blind to.
        for (unsigned i = 0; i < 3000; i++) {
            const uint64_t a = rng.below(span / unit) * unit;
            const uint64_t b = rng.below(span / unit) * unit;
            const uint64_t lo = a < b ? a : b;
            const uint64_t hi = (a < b ? b : a) + unit - 1;

            rdx::Mapping* v = rng.chance(60) ? makeMapping(lo) : nullptr;
            const auto st = f->apply(lo, hi, v);
            ASSERT_TRUE(st == rdx::ApplyStatus::Ok);
            quiesce(h);
            model.apply(lo, hi, v);
            // Validate EVERY step, not just the end: a reference-count defect
            // detected 3000 operations later is a bisect, not a diagnosis.
            ValidT::validate(f.tree, "churn step");
        }

        assertMatchesModel(f.tree, model, "churn");
        ValidT::validate(f.tree, "churn");

        // Clear everything: the tree collapses to just its root, and every
        // record's last naming slot releases.
        ASSERT_TRUE(f->apply(0, span - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        ASSERT_EQ(size_t{1}, f->nodeCount());
        ASSERT_EQ(size_t{0}, liveCountOf<rdx::Mapping>());
        ValidT::validate(f.tree, "drained");
    }
    // The fixture destroyed the root; nothing may remain.
    assertBaseline(baseline, "churn cycle");
}
