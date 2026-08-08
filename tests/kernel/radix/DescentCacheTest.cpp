//
// radix-tree Phase 4 — the descent cache (§7.5; DEC-016 / 078 / 079 / 096).
//
// §13's Phase 4 gate is four properties and a report:
//
//   - cache-fill then unmap-and-remap from another CPU — the ABA shape DEC-016
//     exists for;
//   - the acquire-side target: a reference acquired only in the observing
//     section, plus the close-then-reclaim-then-install case;
//   - the teardown-survivor target: tear down while a foreign CPU holds an
//     interior-node pin, then hit — the entry must observe the mark. §11 is
//     explicit that the quiesced-tree validator "checks the opposite polarity
//     and cannot reach this, since after teardown there is no root to walk
//     from";
//   - every-node-of-a-detached-subtree-marked, driven through an interior-node
//     cache entry. §11: "Root-only marking passes every other check."
//
// Two of those are gates on Phase 3's teardown walk rather than on this file,
// and that is the point: the marking walk landed in Phase 3 *because* the cache
// is the only thing that can observe its absence.
//
// The failure modes here are quiet ones. The cache's characteristic bug is not a
// crash but a `Mapping` returned for the wrong address, so nearly every test
// below asserts an ANSWER and not merely a status.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/AddressSpace.h>
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/DescentCache.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA    = rdx::HarnessSlotCodec<GA>;
using BlockA    = rdx::ControlBlock<GA, CodecA>;
using FreelistA = rdx::ControlBlockFreelist<GA, CodecA>;
using TreeA     = rdx::CoreTree<GA, CodecA>;
using CacheA    = rdx::DescentCache<GA, CodecA>;
// The concrete node type at the deepest level, for the accounting assertions —
// aliased because a template argument list inside ASSERT_TRUE is a comma the
// preprocessor splits on.
using LeafNodeA = rdx::Node<GA, rdx::valence(GA, GA.levelCount)>;

constexpr uint64_t kPage = 4096;
constexpr unsigned kTestRecords = rdx::deferredReleaseBound(GA);

// The amd64 geometry's deepest node spans one C-level slot array of 4 KiB
// slots: 16 x 4 KiB = 64 KiB. That is the default pin level's node, and
// therefore the default entry's VA range and the cache's index granularity.
constexpr uint64_t kLeafNodeSpan = rdx::nodeSpan(GA, GA.levelCount);
static_assert(kLeafNodeSpan == 64 * 1024);
// The node one level up — the "interior node" §11's marking target wants an
// entry on.
constexpr uint64_t kInteriorSpan = rdx::nodeSpan(GA, GA.levelCount - 1);
static_assert(kInteriorSpan == 1024 * 1024);

// **Leaves live at any level, so a tree's depth is a property of its CONTENTS.**
// A single 4 KiB mapping does not produce a deep tree: at level 4 the range
// field's unit is already 4 KiB (resolutionShortfall(GA, 4) == 0), so one page
// is written as a leaf in a level-4 slot and the descent terminates two nodes
// down. Every test here that wants an entry on a *deep* node therefore maps a
// PAIR of adjacent pages: two leaves inside one 64 KiB level-5 slot force
// subdivision to the floor, and the descent then reaches a level-6 node whose
// span is `kLeafNodeSpan`.
//
// Worth stating rather than working around silently, because the first version
// of these tests asserted entry spans against `kLeafNodeSpan` while mapping
// single pages, and every one of those assertions was measuring a 32 MiB
// level-4 node.
static_assert(rdx::resolutionShortfall(GA, GA.levelCount - 2) == 0,
              "the pair-mapping trick assumes a single page lands at level 4; if the "
              "geometry's shortfall changes, the fixtures' depth assumptions change with it");

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// The bare arena — an address space brings its own domain and pools, and a
// second set from RadixHarness would make every accounting assertion ambiguous.
// Same shape as AddressSpaceTest's.
struct BareArena {
    explicit BareArena(size_t cpus = 1) {
        VS::test::initialize(cpus, 1);
        numa::test::configure(cpus, 1, &allToDomainZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        rdx::HarnessSlotBase::bind(
            static_cast<uintptr_t>(VS::arenaVirtualBase(0).value), kMockArenaBytes);
        resetAccounting();
        resetInjection();
    }
    ~BareArena() {
        kernel::rcu::test::resetDomainManagementState();
        resetInjection();
        VS::test::shutdown();
    }
};

// An address space with one cluster covering [0, span), ready to be mapped into.
struct Space {
    FreelistA freelist;
    BlockA*   block = nullptr;

    explicit Space(size_t cpus = 1, uint64_t span = uint64_t{1} << 30) {
        if (rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, cpus,
                                    kTestRecords, block) != rdx::CreateStatus::Ok) {
            throw AssertionFailure("descent cache: address-space creation failed");
        }
        if (block->clusters.ensureCovers(0, span - 1) != rdx::ClusterStatus::Ok) {
            throw AssertionFailure("descent cache: cluster creation failed");
        }
    }
    ~Space() { if (block) rdx::destroyAddressSpace(freelist, block); }

    [[nodiscard]] TreeA treeFor(uint64_t va) {
        return block->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
    }

    void map(uint64_t lo, uint64_t hi, rdx::Mapping* m) {
        TreeA t = treeFor(lo);
        if (t.apply(lo, hi, m) != rdx::ApplyStatus::Ok) {
            throw AssertionFailure("descent cache: apply failed");
        }
    }

    // Map one page and return the record, so a test can compare identities
    // rather than merely "something was found".
    rdx::Mapping* mapPage(uint64_t va) {
        rdx::Mapping* m = makeMapping(va);
        if (!m) throw AssertionFailure("descent cache: Mapping allocation failed");
        map(va, va + kPage - 1, m);
        return m;
    }

    // Two adjacent pages, which is what forces subdivision to the floor. See the
    // note above `kLeafNodeSpan`. Returns the record covering `va`.
    rdx::Mapping* mapPair(uint64_t va) {
        rdx::Mapping* first = mapPage(va);
        (void)mapPage(va + kPage);
        return first;
    }

    void unmap(uint64_t lo, uint64_t hi) { map(lo, hi, nullptr); }

    void quiesceSpace() { (void)kernel::rcu::drainAllQuiescent(block->domain); }
};

// Fill an entry for `va`: the install is VA-range-deferred, so the first miss
// only records a candidate and the second installs. Returns the number of
// lookups it took, asserted by callers that care.
void fill(CacheA& cache, BlockA& block, uint64_t va) {
    (void)cache.lookup(block, va);   // miss: candidate recorded
    (void)cache.lookup(block, va);   // miss: install, inside this descent
}

uint64_t stat(const Atomic<uint64_t>& a) { return a.load(rdx::kCacheAccounting); }

}  // namespace

// ─── The install policy (DEC-079, §7.3's admissible form) ──────────────────

TEST(radix_cache_install_is_deferred_by_one_miss) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheA>();

    const uint64_t va = 4 * kLeafNodeSpan;
    rdx::Mapping* m = space.mapPair(va);

    // First sight: a candidate range, no atomic, no entry. §7.3's rule is that
    // "no atomic is paid on first sight" — which is also why the alternative
    // (remember the node pointer now, install it later) is so tempting and so
    // wrong.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == m);
    }
    ASSERT_TRUE(stat(cache->stats().installs) == 0);
    ASSERT_TRUE(stat(cache->stats().pinAcquires) == 0);
    ASSERT_TRUE(stat(cache->stats().candidatesRecorded) == 1);
    ASSERT_TRUE(!cache->entryOccupied(cache->indexFor(va)));

    // Second sight inside that range: the install, paid inside THIS descent's
    // section.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == m);
    }
    ASSERT_TRUE(stat(cache->stats().installs) == 1);
    ASSERT_TRUE(stat(cache->stats().pinAcquires) == 1);
    ASSERT_TRUE(cache->entryOccupied(cache->indexFor(va)));
    ASSERT_TRUE(stat(cache->stats().hits) == 0);
    // The entry covers the pinned node's whole span, and the fixture is deep
    // enough for that to be the floor-level node rather than an ancestor.
    {
        const unsigned idx = cache->indexFor(va);
        ASSERT_TRUE(cache->entryHi(idx) - cache->entryLo(idx) + 1 == kLeafNodeSpan);
    }

    // Third: the hit. One acquire load of the mark and a resumed descent — and
    // no further atomic, which is DEC-016's cost claim ("atomics fall on cache
    // turnover rather than on faults").
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == m);
    }
    ASSERT_TRUE(stat(cache->stats().hits) == 1);
    ASSERT_TRUE(stat(cache->stats().pinAcquires) == 1);
    ASSERT_TRUE(stat(cache->stats().freshnessLoads) == 1);

    cache->evictAllOnThisCpu();
}

// A hit is a descent SHORTCUT, not a memo of one address: the entry covers the
// pinned node's whole span, so every address under it hits — and an address
// under it that is NOT mapped must still answer "unmapped" rather than
// returning the neighbour.
TEST(radix_cache_hit_answers_for_the_whole_node_span) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheA>();

    const uint64_t base = 7 * kLeafNodeSpan;
    rdx::Mapping* first = space.mapPage(base);
    rdx::Mapping* last  = space.mapPage(base + kLeafNodeSpan - kPage);

    fill(*cache, *space.block, base);

    {
        auto r = cache->lookup(*space.block, base + kLeafNodeSpan - kPage);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == last);
    }
    {
        auto r = cache->lookup(*space.block, base + kPage);   // a hole
        ASSERT_TRUE(!static_cast<bool>(r));
    }
    {
        auto r = cache->lookup(*space.block, base);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == first);
    }
    ASSERT_TRUE(stat(cache->stats().hits) == 3);

    cache->evictAllOnThisCpu();
}

// ─── §11: every node of a detached subtree is marked ───────────────────────
//
// "Fill a descent-cache entry on an INTERIOR node, MAP_FIXED-replace the
// subtree, then hit the cache: the entry must observe the mark. Root-only
// marking passes every other check."
//
// The entry here is on the deepest node, which sits strictly inside the 1 MiB
// subtree the replacement detaches — so a walk that marked only the subtree's
// root would leave this entry pointed at an unmarked node whose children have
// been freed, and the hit would descend into them.
TEST(radix_cache_detached_subtree_is_observed_through_an_interior_entry) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheA>();

    const uint64_t region = kInteriorSpan;         // one whole level-5 subtree
    const uint64_t va     = region + kLeafNodeSpan;
    rdx::Mapping* original = space.mapPair(va);
    // A second populated node elsewhere in the same subtree, so the replacement
    // has a genuine subtree to detach rather than a single node.
    (void)space.mapPair(va + 4 * kLeafNodeSpan);

    fill(*cache, *space.block, va);
    ASSERT_TRUE(stat(cache->stats().installs) == 1);
    // The pinned node is the floor-level one, strictly INSIDE the subtree the
    // replacement below detaches — which is what makes root-only marking fail
    // this test and pass every other.
    {
        const unsigned idx = cache->indexFor(va);
        ASSERT_TRUE(cache->entryHi(idx) - cache->entryLo(idx) + 1 == kLeafNodeSpan);
    }
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == original);
    }

    // MAP_FIXED over the whole subtree. Every node beneath it is detached, and
    // invariant 23 says every one of them carries the mark.
    rdx::Mapping* replacement = makeMapping(region);
    ASSERT_TRUE(replacement != nullptr);
    space.map(region, region + region - 1, replacement);

    // The hit must observe the mark, fall back, and produce the NEW answer.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r));
        ASSERT_TRUE(r.mapping() == replacement);
    }
    ASSERT_TRUE(stat(cache->stats().detachedEvictions) == 1);
    ASSERT_TRUE(!cache->entryOccupied(cache->indexFor(va)));

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// The same property driven at a shallower pin level, so the ENTRY itself names
// an interior node rather than the deepest one. DEC-078's "caller-chosen level"
// is what makes this expressible, and §11 asks for the interior case by name.
TEST(radix_cache_interior_level_entry_observes_the_mark) {
    BareArena arena;
    Space     space;
    // Pin at the level whose nodes span 1 MiB.
    auto cache = std::make_unique<CacheA>(GA.levelCount - 1);

    const uint64_t region = 3 * kInteriorSpan;
    const uint64_t va     = region + kLeafNodeSpan;
    rdx::Mapping* original = space.mapPair(va);

    fill(*cache, *space.block, va);
    ASSERT_TRUE(stat(cache->stats().installs) == 1);
    // The entry covers a whole 1 MiB node, not a 64 KiB one.
    const unsigned idx = cache->indexFor(va);
    ASSERT_TRUE(cache->entryHi(idx) - cache->entryLo(idx) + 1 == kInteriorSpan);
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == original);
    }

    // Replace the cluster root's whole first slot (32 MiB), so the level-4 node
    // is the detachment root and the pinned level-5 node is strictly inside the
    // subtree rather than being its root.
    rdx::Mapping* replacement = makeMapping(0);
    ASSERT_TRUE(replacement != nullptr);
    space.map(0, rdx::slotSpan(GA, GA.defaultRootLevel) - 1, replacement);

    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == replacement);
    }
    ASSERT_TRUE(stat(cache->stats().detachedEvictions) == 1);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── §13: cache-fill, then unmap-and-remap from another CPU ────────────────
//
// The ABA shape DEC-016 exists for. The unmap reclaims the cached node and its
// slab slot is recycled; the remap builds a fresh node, plausibly at the very
// same address. Without the counted reference the entry's pointer would come to
// name a live, unmarked, unrelated node — and the failure is not a crash, it is
// a `Mapping` for the wrong address. Without the mark the entry would resume
// into a node whose children are freed.
TEST(radix_cache_unmap_and_remap_from_another_cpu) {
    constexpr size_t kCpus = 2;
    BareArena arena(kCpus);
    Space     space(kCpus);
    auto      cache = std::make_unique<CacheA>();

    const uint64_t va = 9 * kLeafNodeSpan;
    rdx::Mapping* original = space.mapPair(va);
    fill(*cache, *space.block, va);
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == original);
    }
    ASSERT_TRUE(stat(cache->stats().hits) == 1);

    // CPU 1 unmaps the region — reclaiming the cached node — and remaps it.
    rdx::Mapping* replacement = nullptr;
    std::thread other([&] {
        kernel::test::bindThreadToCpu(1);
        space.unmap(va, va + kLeafNodeSpan - 1);
        // Drain so the reclaimed node's grace period completes and its slab slot
        // is genuinely available for the remap to recycle. Without this the ABA
        // window the test is aiming at may simply not open.
        (void)kernel::rcu::drainAllQuiescent(space.block->domain);
        replacement = makeMapping(va);
        if (replacement) space.map(va, va + kPage - 1, replacement);
        (void)space.mapPage(va + kPage);   // re-deepen, as mapPair does
        kernel::test::bindThreadToCpu(1);
    });
    other.join();
    kernel::test::bindThreadToCpu(0);
    ASSERT_TRUE(replacement != nullptr);
    ASSERT_TRUE(replacement != original);

    // The hit must not answer from the recycled node.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r));
        ASSERT_TRUE(r.mapping() == replacement);
    }
    ASSERT_TRUE(stat(cache->stats().detachedEvictions) == 1);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── §11: no reference is acquired outside the observing section ───────────

TEST(radix_cache_pin_outside_a_section_is_rejected) {
    BareArena arena;
    Space     space;

    const uint64_t va = 2 * kLeafNodeSpan;
    (void)space.mapPair(va);

    TreeA t = space.treeFor(va);
    typename TreeA::PinSite site;
    {
        kernel::rcu::ReadGuard guard(space.block->domain);
        (void)t.descendLocked(t.currentBinding(), va, TreeA::kPinAtDeepest, &site);
        ASSERT_TRUE(site.valid);
    }
    // The section is closed. §7.3: "a counted reference to a node is acquired
    // only inside the read section in which that node's link was observed."
    EXPECT_ASSERT_FAILURE((void)t.pinLocked(site));

    space.quiesceSpace();
}

// §11's second half of the acquire-side target: "a case that fills a cache
// candidate, closes the section, and reclaims the node before the install would
// run."
//
// This is the test that distinguishes DEC-079's admissible install from the
// inadmissible one. Only the VA RANGE crossed the boundary, so the node being
// reclaimed in between is a non-event: the second miss re-descends from the root
// and pins whatever is there now. Had the implementation deferred the POINTER,
// the recycled slab slot would be pinned instead — a live, unmarked, unrelated
// node whose mark check passes.
TEST(radix_cache_candidate_survives_reclamation_of_its_node) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheA>();

    const uint64_t va = 11 * kLeafNodeSpan;
    rdx::Mapping* first = space.mapPair(va);

    // First miss: the candidate.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == first);
    }
    ASSERT_TRUE(stat(cache->stats().candidatesRecorded) == 1);
    ASSERT_TRUE(stat(cache->stats().pinAcquires) == 0);

    // Reclaim the node the candidate described, and let its grace period
    // complete so its memory is genuinely recycled.
    space.unmap(va, va + kLeafNodeSpan - 1);
    space.quiesceSpace();

    // Re-populate with a different record, forcing a new node into (plausibly)
    // the recycled slot.
    rdx::Mapping* second = space.mapPair(va);
    ASSERT_TRUE(second != first);

    // The install now happens against a freshly-descended node.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r));
        ASSERT_TRUE(r.mapping() == second);
    }
    ASSERT_TRUE(stat(cache->stats().installs) == 1);

    // ...and the entry it installed answers correctly.
    {
        auto r = cache->lookup(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == second);
    }
    ASSERT_TRUE(stat(cache->stats().hits) == 1);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── §11: teardown leaves no unmarked survivor ─────────────────────────────
//
// "Tear down while a foreign CPU holds a cache reference to an interior node,
// then hit that cache." The teardown walk marks every node (DEC-100) precisely
// so this entry can learn its node is gone; a walk that unlinked without marking
// would leave the entry pointed at a node that is unlinked, unmarked, alive, and
// holding slots that point at freed children.
//
// The address-space sequence is driven step by step rather than through
// `destroyAddressSpace`, because the observation has to happen between the walk
// and the block's reuse — which is exactly the window DEC-096 calls residue.
TEST(radix_cache_teardown_survivor_observes_the_mark) {
    constexpr size_t kCpus = 2;
    BareArena arena(kCpus);
    FreelistA freelist;
    BlockA*   block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, kCpus,
                                        kTestRecords, block) == rdx::CreateStatus::Ok);
    auto cache = std::make_unique<CacheA>();

    const uint64_t va = 5 * kLeafNodeSpan;
    ASSERT_TRUE(block->clusters.ensureCovers(0, (uint64_t{1} << 30) - 1)
                == rdx::ClusterStatus::Ok);
    rdx::Mapping* m = makeMapping(va);
    rdx::Mapping* neighbour = makeMapping(va + kPage);
    ASSERT_TRUE(m != nullptr && neighbour != nullptr);
    {
        TreeA t = block->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
        ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);
        // The pair, so the pinned node is the floor-level one — see the note on
        // kLeafNodeSpan.
        ASSERT_TRUE(t.apply(va + kPage, va + 2 * kPage - 1, neighbour)
                    == rdx::ApplyStatus::Ok);
    }

    // The foreign CPU fills its own row of the cache.
    std::thread filler([&] {
        kernel::test::bindThreadToCpu(1);
        fill(*cache, *block, va);
        auto r = cache->lookup(*block, va);
        if (!r || r.mapping() != m) throw AssertionFailure("filler: wrong answer");
    });
    filler.join();
    kernel::test::bindThreadToCpu(0);
    ASSERT_TRUE(stat(cache->stats().installs) == 1);

    // §7.4's sequence, up to and including the walk. The threads are joined, so
    // the no-new-users precondition holds.
    block->dying.store(1, rdx::kDyingFlagStore);
    kernel::rcu::synchronize(block->domain);
    block->clusters.tearDownClusters();
    (void)kernel::rcu::drainAllQuiescent(block->domain);

    // Every node is retired and its deleter has run — but the pinned one is
    // still alive on the cache's count. That is DEC-096's residue, and the
    // teardown deliberately did no cache work to remove it.
    ASSERT_TRUE(liveCountOf<LeafNodeA>() > 0);

    // The hit. The generation still matches — the block has not been recycled —
    // so the mark is what has to answer, and this is the case §11 says the
    // quiesced-tree validator cannot reach.
    std::thread hitter([&] {
        kernel::test::bindThreadToCpu(1);
        auto r = cache->lookup(*block, va);
        if (r) throw AssertionFailure("teardown survivor: a hit answered from a dead tree");
    });
    hitter.join();
    kernel::test::bindThreadToCpu(0);
    ASSERT_TRUE(stat(cache->stats().detachedEvictions) == 1);
    // The eviction released the last reference, so the residue is home.
    ASSERT_TRUE(stat(cache->stats().releasesThatDestroyed) == 1);

    // The rest of §7.4.
    block->clusters.freeRootPage();
    block->pools.destroy();
    (void)block->domain.deinit();
    {
        kernel::rcu::DomainManagementLockGuard guard;
        freelist.returnLocked(block);
    }
    assertNoLiveObjects("teardown survivor");
}

// ─── DEC-096: a dead address space's entries are harmless residue ──────────
//
// The generation check is the ENTIRE safety story: no teardown-time cache work,
// no IPIs. So a control block recycled into a new address space must produce a
// generation mismatch, and the mismatch must release the pin rather than merely
// ignore it — the release is what turns the residue bound into a bound rather
// than a leak.
TEST(radix_cache_generation_mismatch_evicts_dead_residue) {
    BareArena arena;
    FreelistA freelist;
    auto      cache = std::make_unique<CacheA>();
    const uint64_t va = 6 * kLeafNodeSpan;

    uint64_t firstGeneration = 0;
    const BlockA* firstAddress = nullptr;
    {
        BlockA* block = nullptr;
        ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                            kTestRecords, block) == rdx::CreateStatus::Ok);
        firstGeneration = block->generation;
        firstAddress    = block;
        ASSERT_TRUE(block->clusters.ensureCovers(0, (uint64_t{1} << 30) - 1)
                    == rdx::ClusterStatus::Ok);
        rdx::Mapping* m = makeMapping(va);
        ASSERT_TRUE(m != nullptr);
        TreeA t = block->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
        ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);

        fill(*cache, *block, va);
        ASSERT_TRUE(stat(cache->stats().installs) == 1);

        rdx::destroyAddressSpace(freelist, block);
    }
    // The entry survives its address space. Nothing invalidated it, by design.
    ASSERT_TRUE(cache->entryOccupied(cache->indexFor(va)));

    {
        BlockA* reused = nullptr;
        ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                            kTestRecords, reused) == rdx::CreateStatus::Ok);
        // The freelist handed the same storage back with a fresh generation —
        // which is the whole reason the identity check is a monotonic counter
        // and not the block's address.
        ASSERT_TRUE(reused == firstAddress);
        ASSERT_TRUE(reused->generation != firstGeneration);

        ASSERT_TRUE(reused->clusters.ensureCovers(0, (uint64_t{1} << 30) - 1)
                    == rdx::ClusterStatus::Ok);
        rdx::Mapping* m2 = makeMapping(va);
        ASSERT_TRUE(m2 != nullptr);
        {
            TreeA t = reused->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
            ASSERT_TRUE(t.apply(va, va + kPage - 1, m2) == rdx::ApplyStatus::Ok);
        }

        auto r = cache->lookup(*reused, va);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == m2);
        ASSERT_TRUE(stat(cache->stats().generationEvictions) == 1);
        ASSERT_TRUE(stat(cache->stats().releasesThatDestroyed) == 1);
        ASSERT_TRUE(!cache->entryOccupied(cache->indexFor(va)) ||
                    cache->entryGeneration(cache->indexFor(va)) == reused->generation);

        rdx::destroyAddressSpace(freelist, reused);
    }
    cache->evictAllOnThisCpu();
    assertNoLiveObjects("generation mismatch");
}

// ─── §7.5: the eviction's zero-observing destroy runs inside the section ───
//
// The Phase 4 work item asks for this to be exercised deliberately, and the
// reason it needs its own test is that "where" leaves no trace in any counter.
// §7.5 spends a paragraph arguing the call is legal — "a refcount-zero destroy
// is the DESTRUCTOR, not a deleter, so §7.6's deleters-run-outside-any-section
// rule does not govern it; invariant 24 makes the destructor release nothing, so
// it is bounded straight-line work; and vmsmalloc DEC-014 permits `vmsfree` from
// `#PF`" — and an implementation that quietly moved the release outside the
// section would pass every other test in this file.
//
// So the oracle grows a destroy observer, and the observer asks the RCU domain
// the one question that matters at that instant.
namespace {

std::atomic<size_t>   gDestroysSeen{0};
std::atomic<size_t>   gDestroysInsideSection{0};
kernel::rcu::Domain*  gWatchedDomain = nullptr;

void watchDestroys(size_t) {
    gDestroysSeen.fetch_add(1, std::memory_order_relaxed);
    if (gWatchedDomain != nullptr &&
        kernel::rcu::test::inSection(
            *gWatchedDomain, static_cast<size_t>(arch::getCurrentProcessorID()))) {
        gDestroysInsideSection.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

TEST(radix_cache_eviction_destroy_runs_inside_the_descent_section) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheA>();

    // Two VAs that collide on the same direct-mapped index, so installing the
    // second evicts the first. `Entries` is 4 and the index is VA bits above the
    // pinned node's span, so four node spans apart lands on the same entry.
    const uint64_t vaA = 16 * kLeafNodeSpan;
    const uint64_t vaB = vaA + 4 * kLeafNodeSpan;
    ASSERT_TRUE(cache->indexFor(vaA) == cache->indexFor(vaB));

    (void)space.mapPair(vaA);
    rdx::Mapping* mB = space.mapPair(vaB);

    fill(*cache, *space.block, vaA);
    ASSERT_TRUE(cache->entryOccupied(cache->indexFor(vaA)));

    // Reclaim A's node and complete its grace period. Its deleter's self-release
    // takes the structural count away, leaving ONLY the cache's pin — so the
    // next release is the zero-observing one.
    space.unmap(vaA, vaA + kLeafNodeSpan - 1);
    space.quiesceSpace();

    // Arm the observer and drive B's install. The install's eviction of A
    // happens inside the descent's open ReadGuard, by construction — this is
    // what asserts it actually does.
    gDestroysSeen.store(0, std::memory_order_relaxed);
    gDestroysInsideSection.store(0, std::memory_order_relaxed);
    gWatchedDomain = &space.block->domain;
    setDestroyObserver(&watchDestroys);

    (void)cache->lookup(*space.block, vaB);   // candidate
    (void)cache->lookup(*space.block, vaB);   // install -> evicts A -> destroys

    clearDestroyObserver();
    gWatchedDomain = nullptr;

    ASSERT_TRUE(stat(cache->stats().installEvictions) == 1);
    ASSERT_TRUE(stat(cache->stats().releasesThatDestroyed) == 1);
    ASSERT_TRUE(gDestroysSeen.load(std::memory_order_relaxed) == 1);
    ASSERT_TRUE(gDestroysInsideSection.load(std::memory_order_relaxed) == 1);

    // ...and the entry that displaced it works.
    {
        auto r = cache->lookup(*space.block, vaB);
        ASSERT_TRUE(static_cast<bool>(r) && r.mapping() == mB);
    }
    ASSERT_TRUE(stat(cache->stats().hits) == 1);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── Instrumentation and calibration (the Phase 4 work item) ───────────────
//
// "Hit rate, atomic counts, per-entry thrash counters — the calibration inputs
// for entry count and install threshold (both Provisional)."
//
// The workload is DEC-079's own argument made executable: alternating hot
// regions. A single entry thrashes on them deterministically — that is the
// stated reason the set exists — so the useful measurement is the hit rate as a
// function of entry count over exactly that pattern, plus the thrash counters
// that say WHERE the pressure lands.
namespace {

template <unsigned Entries>
struct SweepResult {
    uint64_t lookups = 0, hits = 0, installs = 0, pinAtomics = 0, thrash = 0;
};

// `regions` hot regions, visited round-robin, `rounds` times each. Every VA is
// mapped, so a miss is a genuine cache miss and never an unmapped answer.
template <unsigned Entries>
SweepResult<Entries> sweep(unsigned regions, unsigned rounds, unsigned threshold) {
    BareArena arena;
    Space     space;
    auto cache = std::make_unique<
        rdx::DescentCache<GA, CodecA, rdx::kDetachBudget, Entries>>(
            TreeA::kPinAtDeepest, threshold);

    // Spaced one node span apart so consecutive regions land on consecutive
    // entries — the worst case for a small set and the pattern the design names.
    for (unsigned r = 0; r < regions; r++) {
        (void)space.mapPair(uint64_t{32 + r} * kLeafNodeSpan);
    }
    for (unsigned k = 0; k < rounds; k++) {
        for (unsigned r = 0; r < regions; r++) {
            (void)cache->lookup(*space.block, uint64_t{32 + r} * kLeafNodeSpan);
        }
    }

    SweepResult<Entries> out;
    out.lookups    = stat(cache->stats().lookups);
    out.hits       = stat(cache->stats().hits);
    out.installs   = stat(cache->stats().installs);
    out.pinAtomics = stat(cache->stats().pinAcquires) + stat(cache->stats().pinReleases);
    for (unsigned i = 0; i < Entries; i++) out.thrash += stat(cache->stats().entryThrash[i]);
    cache->evictAllOnThisCpu();
    space.quiesceSpace();
    return out;
}

}  // namespace

TEST(radix_cache_calibration_report) {
    constexpr unsigned kRounds = 64;
    std::printf("\n  descent cache — hit rate vs. entry count "
                "(%u rounds over N alternating hot regions, threshold 2)\n", kRounds);
    std::printf("      regions | entries |  hit%% | installs | pin atomics | thrash\n");

    // The headline comparison DEC-079 rests on: two alternating regions against
    // a one-entry cache.
    const auto one2  = sweep<1>(2, kRounds, 2);
    const auto four2 = sweep<4>(2, kRounds, 2);
    std::printf("            2 |       1 | %5.1f | %8llu | %11llu | %6llu\n",
                100.0 * double(one2.hits) / double(one2.lookups),
                (unsigned long long)one2.installs, (unsigned long long)one2.pinAtomics,
                (unsigned long long)one2.thrash);
    std::printf("            2 |       4 | %5.1f | %8llu | %11llu | %6llu\n",
                100.0 * double(four2.hits) / double(four2.lookups),
                (unsigned long long)four2.installs, (unsigned long long)four2.pinAtomics,
                (unsigned long long)four2.thrash);

    // Two alternating regions are exactly the pattern a single entry cannot
    // hold, and the pathology is TOTAL rather than merely bad: with one entry
    // the two regions also share one candidate register, each miss overwrites
    // the other's candidate, and the sighting count never reaches the install
    // threshold — so a one-entry cache under this pattern never installs at all
    // and its hit rate is exactly zero. That is a sharper statement of DEC-079's
    // rationale than the entry itself makes, and it is what these two assertions
    // pin down: if a future change made one entry as good as four here, the
    // rationale would have stopped being true and would need re-reading rather
    // than re-tuning.
    ASSERT_TRUE(one2.hits == 0);
    ASSERT_TRUE(one2.installs == 0);
    ASSERT_TRUE(four2.hits > 0);
    // DEC-016's cost claim made checkable: atomics fall on TURNOVER, so a cache
    // that settles pays a bounded number of them regardless of how long the
    // workload runs.
    ASSERT_TRUE(four2.pinAtomics <= 2 * 4);

    for (unsigned regions : {2u, 4u, 8u}) {
        const auto e1 = sweep<1>(regions, kRounds, 2);
        const auto e2 = sweep<2>(regions, kRounds, 2);
        const auto e4 = sweep<4>(regions, kRounds, 2);
        const auto e8 = sweep<8>(regions, kRounds, 2);
        const auto row = [&](unsigned entries, auto& s) {
            std::printf("      %7u | %7u | %5.1f | %8llu | %11llu | %6llu\n",
                        regions, entries, 100.0 * double(s.hits) / double(s.lookups),
                        (unsigned long long)s.installs,
                        (unsigned long long)s.pinAtomics, (unsigned long long)s.thrash);
        };
        row(1, e1); row(2, e2); row(4, e4); row(8, e8);
    }

    // The install threshold, at the entry count DEC-079 provisionally fixes and
    // with the set NOT over-subscribed. 1 is the rejected un-deferred install.
    std::printf("\n  install threshold sweep (4 entries, %u rounds)\n", kRounds);
    std::printf("      regions | threshold |  hit%% | installs | pin atomics | thrash\n");
    const auto thresholdRow = [&](unsigned regions, unsigned t, auto& s) {
        std::printf("      %7u | %9u | %5.1f | %8llu | %11llu | %6llu\n",
                    regions, t, 100.0 * double(s.hits) / double(s.lookups),
                    (unsigned long long)s.installs, (unsigned long long)s.pinAtomics,
                    (unsigned long long)s.thrash);
    };
    for (unsigned t : {1u, 2u, 3u, 4u}) {
        const auto s = sweep<4>(4, kRounds, t);
        thresholdRow(4, t, s);
    }

    // ...and OVER-subscribed, which is where the deferral earns its keep. This
    // is the row worth reading twice.
    const auto over1 = sweep<4>(8, kRounds, 1);
    const auto over2 = sweep<4>(8, kRounds, 2);
    thresholdRow(8, 1, over1);
    thresholdRow(8, 2, over2);
    std::printf("\n");

    // **The finding.** When more hot regions alias onto one entry than it can
    // hold, threshold 1 and threshold 2 fail in opposite directions:
    //
    //   - at 1, every miss installs, so the entry is rewritten on every access.
    //     The pin atomics then scale with the LOOKUP count rather than with
    //     turnover — the exact inversion of DEC-016's cost claim — and the hit
    //     rate is still zero, because the entry is always the wrong one.
    //   - at 2, the two regions overwrite each other's candidate before either
    //     reaches the threshold, so the entry never installs and the cache
    //     silently switches ITSELF OFF for that index: no atomics, no hits, one
    //     compare per lookup.
    //
    // The second is strictly the better failure. It also means the deferred
    // install is doing more work than DEC-079 claims for it — the entry
    // justifies deferral by "no atomic is paid on first sight", and this shows it
    // additionally bounds the atomic cost of an over-subscribed index at zero.
    //
    // Both are per-index and local: an entry whose regions do not alias is
    // unaffected, which is what the 5-regions-over-4-entries row shows.
    ASSERT_TRUE(over2.pinAtomics == 0);
    ASSERT_TRUE(over1.pinAtomics > over1.installs);        // rewritten, not settled
    ASSERT_TRUE(over1.pinAtomics > 4 * kRounds);           // scales with lookups
    ASSERT_TRUE(over1.thrash > 0);                          // and the counter says so

    std::printf("  degradation is per-index, not global (threshold 2, 4 entries):\n");
    std::printf("      regions | hit%% | installs | pin atomics | thrash\n");
    const auto five = sweep<4>(5, kRounds, 2);
    std::printf("      %7u | %4.1f | %8llu | %11llu | %6llu\n\n", 5u,
                100.0 * double(five.hits) / double(five.lookups),
                (unsigned long long)five.installs, (unsigned long long)five.pinAtomics,
                (unsigned long long)five.thrash);
    // Three of the four entries settle; the oversubscribed one switches off. So
    // the hit rate is bounded well away from both 0 and 100 — "the set degrades
    // gracefully" is a claim, and this is the number behind it.
    ASSERT_TRUE(five.hits > 0);
    ASSERT_TRUE(five.installs == 3);
}

// ─── The cache under genuine concurrency ───────────────────────────────────
//
// Everything above drives the cache from one thread at a time, because each of
// those properties is about a specific interleaving that has to be arranged
// rather than hoped for. This is the opposite: real readers on real CPUs against
// real writers, with the ONE invariant that catches the cache's characteristic
// failure.
//
// That failure is not a crash. §7.3 spells it out — a cache that resumed from a
// recycled or detached node "does not crash, it returns a `Mapping` for the
// wrong address" — so the check is on the ANSWER: every non-null result must
// cover the address that was asked for, and the record it names must be the one
// placed at that range. A resume in an unrelated part of the tree produces a
// perfectly well-formed result whose range sits somewhere else entirely.
TEST(radix_cache_concurrent_readers_never_answer_for_the_wrong_address) {
    constexpr size_t   kCpus     = 4;
    constexpr unsigned kRegions  = 24;
    constexpr unsigned kOps      = 3000;

    BareArena arena(kCpus);
    Space     space(kCpus);
    auto      cache = std::make_unique<CacheA>();

    for (unsigned r = 0; r < kRegions; r++) {
        (void)space.mapPair(uint64_t{64 + r} * kLeafNodeSpan);
    }

    std::atomic<bool>   stop{false};
    std::atomic<size_t> wrongAddress{0};
    std::atomic<size_t> wrongRecord{0};
    std::atomic<size_t> answered{0};
    std::vector<std::thread> workers;

    // CPUs 0 and 1 read through the cache.
    for (size_t c = 0; c < 2; c++) {
        workers.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            uint64_t x = 0x9E3779B97F4A7C15ull * (c + 1);
            while (!stop.load(std::memory_order_acquire)) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const uint64_t va =
                    uint64_t{64 + (x % kRegions)} * kLeafNodeSpan + ((x >> 20) % 2) * kPage;
                auto r = cache->lookup(*space.block, va);
                if (!r) continue;
                answered.fetch_add(1, std::memory_order_relaxed);
                // Two independent ways of catching a resume that landed in the
                // wrong part of the tree.
                //
                //   1. The decoded absolute range is computed from the node base
                //      the descent started at, so a resume from an unrelated node
                //      produces a range around THAT node — and the queried
                //      address falls outside it.
                //   2. Every record in this workload is created at a VA inside
                //      one 64 KiB region and can only ever be named by leaves
                //      inside it (a subdivision shrinks a leaf's range but never
                //      moves it out of the record's span). So the record's own
                //      base must sit in the same region as the address asked
                //      for. Note this is deliberately NOT "baseVA == lo": a
                //      subdivided mapping legitimately survives in a leaf whose
                //      range starts after its base, which is invariant 3.
                if (va < r.lo() || va > r.hi()) {
                    wrongAddress.fetch_add(1, std::memory_order_relaxed);
                } else if (r.mapping()->baseVA / kLeafNodeSpan != va / kLeafNodeSpan) {
                    wrongRecord.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // CPUs 2 and 3 mutate: single-page rewrites, whole-node unmaps, and coarse
    // MAP_FIXED replacements — the three shapes that respectively rewrite a
    // slot, reclaim a cached node, and detach a subtree out from under one.
    for (size_t c = 2; c < kCpus; c++) {
        workers.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            uint64_t x = 0xD1B54A32D192ED03ull * (c + 1);
            for (unsigned k = 0; k < kOps; k++) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const uint64_t region = uint64_t{64 + (x % kRegions)} * kLeafNodeSpan;
                switch ((x >> 32) % 3) {
                case 0: {                       // rewrite one page
                    const uint64_t va = region + ((x >> 40) % 2) * kPage;
                    rdx::Mapping* m = makeMapping(va);
                    if (m) space.map(va, va + kPage - 1, m);
                    break;
                }
                case 1:                          // reclaim the node
                    space.unmap(region, region + kLeafNodeSpan - 1);
                    break;
                case 2: {                        // detach the subtree
                    rdx::Mapping* m = makeMapping(region);
                    if (m) space.map(region, region + kLeafNodeSpan - 1, m);
                    break;
                }
                }
            }
        });
    }

    for (size_t c = 2; c < kCpus; c++) workers[c].join();
    stop.store(true, std::memory_order_release);
    for (size_t c = 0; c < 2; c++) workers[c].join();
    kernel::test::bindThreadToCpu(0);

    // The readers must have actually seen something; a run where every lookup
    // fell in a hole would pass vacuously.
    ASSERT_TRUE(answered.load() > 0);
    ASSERT_TRUE(wrongAddress.load() == 0);
    ASSERT_TRUE(wrongRecord.load() == 0);
    // ...and the cache must have been doing its job rather than missing every
    // time, or this would be a test of the plain descent path.
    ASSERT_TRUE(stat(cache->stats().hits) > 0);
    // The eviction is load-bearing here, not incidental: every reclaimed or
    // detached node the readers had cached had to be noticed.
    ASSERT_TRUE(stat(cache->stats().detachedEvictions) > 0);

    for (size_t c = 0; c < kCpus; c++) {
        kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
        cache->evictAllOnThisCpu();
    }
    kernel::test::bindThreadToCpu(0);
    space.quiesceSpace();
}
