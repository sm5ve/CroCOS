//
// Referee-pass adversarial tests for the borrow-shaped lookup (Item B).
//
// Three attacks on the branch's safety argument, each one a hypothesized
// hazard driven to an observable outcome rather than argued:
//
//   1. NESTED COMPOSITION: a BorrowWindow constructed while an OUTER read
//      section is already open. The window's PumpOnExit then runs pumpIfWork
//      with the outer section still open — the exact sequence §7.6 says never
//      happens ("the pump never runs inside a read section"). Nothing asserts
//      against it. The test drives real sealed bags into the pump at that
//      moment and then checks the epoch algebra's promise: nothing this
//      section observed can be reclaimed by that pump, however hard it is
//      driven.
//
//   2. THE §7.5 DESTROY UNDER A LONG CALLER SECTION: a detached-entry borrow
//      hit runs the zero-observing pin release and destroy<Node> inside the
//      CALLER's section, while another borrow from the same window is still
//      outstanding. The outstanding borrow must be unaffected.
//
//   3. USE AFTER THE WINDOW (opt-in, CROCOS_BORROW_UAF_DEMO=1): the contract
//      violation the type cannot stop — carrying a BorrowedLookup across its
//      window's close and dereferencing it after reclamation. Expected outcome
//      is an ASan use-after-poison report from the DEC-052 oracle, i.e. a hard
//      crash. It exists to prove the contract edge is real and that the
//      instrument can see a violation; it is opt-in because its success IS a
//      crash.
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
#include <cstdlib>
#include <memory>
#include <thread>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GB = rdx::kAmd64Geometry;
using CodecB    = rdx::HarnessSlotCodec<GB>;
using BlockB    = rdx::ControlBlock<GB, CodecB>;
using StoreB    = rdx::ControlBlockStore<GB, CodecB>;
using TreeB     = rdx::CoreTree<GB, CodecB>;
using CacheB    = rdx::DescentCache<GB, CodecB>;
using LeafNodeB = rdx::Node<GB, rdx::valence(GB, GB.levelCount)>;

constexpr uint64_t kPage = 4096;
constexpr unsigned kTestRecords = rdx::deferredReleaseBound(GB);
constexpr uint64_t kLeafNodeSpan = rdx::nodeSpan(GB, GB.levelCount);
constexpr uint64_t kInteriorSpan = rdx::nodeSpan(GB, GB.levelCount - 1);

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

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

struct Space {
    StoreB    store;
    BlockB*   block = nullptr;

    explicit Space(size_t cpus = 1, uint64_t span = uint64_t{1} << 30) {
        if (rdx::createAddressSpace(store, kernel::numa::DomainID{0}, cpus,
                                    kTestRecords, block) != rdx::CreateStatus::Ok) {
            throw AssertionFailure("borrow adversarial: address-space creation failed");
        }
        if (block->clusters.ensureCovers(0, span - 1) != rdx::ClusterStatus::Ok) {
            throw AssertionFailure("borrow adversarial: cluster creation failed");
        }
    }
    ~Space() { if (block) rdx::destroyAddressSpace(store, block); }

    [[nodiscard]] TreeB treeFor(uint64_t va) {
        return block->clusters.treeFor(rdx::bucketIndexFor<GB>(va));
    }
    void map(uint64_t lo, uint64_t hi, rdx::Mapping* m) {
        TreeB t = treeFor(lo);
        if (t.apply(lo, hi, m) != rdx::ApplyStatus::Ok) {
            throw AssertionFailure("borrow adversarial: apply failed");
        }
    }
    rdx::Mapping* mapPage(uint64_t va) {
        rdx::Mapping* m = makeMapping(va);
        if (!m) throw AssertionFailure("borrow adversarial: Mapping allocation failed");
        map(va, va + kPage - 1, m);
        return m;
    }
    rdx::Mapping* mapPair(uint64_t va) {
        rdx::Mapping* first = mapPage(va);
        (void)mapPage(va + kPage);
        return first;
    }
    void unmap(uint64_t lo, uint64_t hi) { map(lo, hi, nullptr); }
    void quiesceSpace() { (void)kernel::rcu::drainAllQuiescent(block->domain); }
};

void fill(CacheB& cache, BlockB& block, uint64_t va) {
    (void)cache.lookup(block, va);
    (void)cache.lookup(block, va);
}

uint64_t stat(uint64_t v) { return v; }

}  // namespace

// ─── Attack 1: BorrowWindow nested inside an outer read section ─────────────
//
// The spec sentence under attack (§7.6/DEC-060): "The pump never runs inside a
// read section." BorrowWindow's member ordering closes ITS OWN section before
// pumping — but nothing stops a caller composing the window inside a section
// it already holds, and pumpIfWork carries no in-section assert. This test
// makes that composition, makes the pump have REAL work at the moment it runs
// in-section (a concurrent unmap's retirees, hammered toward expiry by a
// remote CPU), and then requires the two properties that must survive:
//
//   - the borrowed Mapping, observed inside the outer section, is untouched by
//     the in-section pump — the epoch algebra says a section's observations
//     can never be in an expired bag, and this drives that exact edge;
//   - the borrow result stays dereferenceable after the WINDOW closes, because
//     the protecting section is the OUTERMOST one, not the window's.
//
// If this test ever fails at the tag read, the nested composition is not
// benign and the missing assert is a merge blocker rather than a spec-hygiene
// question. Passing does NOT make the composition legal per the spec — it
// demonstrates the violation is silent, which is the referee finding.
TEST(radix_borrow_window_nested_in_outer_section_pump_cannot_reclaim_observations) {
    BareArena arena(2);
    Space     space(2);
    auto      cache = std::make_unique<CacheB>();

    const uint64_t va = 4 * kLeafNodeSpan;
    rdx::Mapping* m = space.mapPair(va);
    const uint64_t backendTag = m->baseVA;
    fill(*cache, *space.block, va);

    std::atomic<bool> borrowed{false};
    std::atomic<bool> unmapped{false};

    std::thread writer([&] {
        kernel::test::bindThreadToCpu(1);
        while (!borrowed.load(std::memory_order_acquire)) {}
        space.unmap(va, va + 2 * kPage - 1);
        // Seal and advance as hard as a remote CPU can: the goal is that when
        // the nested window's PumpOnExit fires inside the outer section, the
        // sealed-bag gate is OPEN and the pump does a full advance-and-sweep
        // rather than gating off. The reader's outer section must still block
        // reclamation of everything it observed.
        for (int i = 0; i < 64; i++) {
            (void)kernel::rcu::tryAdvance(space.block->domain);
            (void)kernel::rcu::drain(space.block->domain);
        }
        unmapped.store(true, std::memory_order_release);
    });

    {
        kernel::rcu::ReadGuard outer(space.block->domain);   // the OUTER section

        rdx::BorrowedLookup b;
        {
            rdx::BorrowWindow window(space.block->domain);   // nested: nesting == 2
            b = cache->borrow(*space.block, va);
            ASSERT_TRUE(static_cast<bool>(b) && b.mapping() == m);
            borrowed.store(true, std::memory_order_release);
            while (!unmapped.load(std::memory_order_acquire)) {}
            // Retirees are pending and remotely sealed. Closing the window now
            // runs pumpIfWork with the OUTER section still open — the §7.6
            // forbidden shape, silently permitted.
        }   // ~BorrowWindow: inner guard closes (nesting 2 -> 1), pump runs IN-SECTION

        // The pump above ran inside our still-open section, with the writer
        // having driven sealing and advances underneath it. If it reclaimed the
        // record this section observed, this read is a use-after-poison and
        // ASan halts the test. The borrow result outliving its WINDOW here is
        // deliberate: the protection was never the window's — it is the
        // outermost section's, which is still open.
        ASSERT_TRUE(b.mapping()->baseVA == backendTag);
        ASSERT_TRUE(liveCountOf<rdx::Mapping>() >= 1);
    }   // outer section closes

    // A caller managing its own guard owes the post-close pump itself
    // (DEC-060's coverage rule) — pay it, then drive reclamation home.
    (void)kernel::rcu::pumpIfWork(space.block->domain);
    writer.join();
    kernel::test::bindThreadToCpu(0);

    space.quiesceSpace();
    ASSERT_TRUE(liveCountOf<rdx::Mapping>() == 0);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── Attack 2: the §7.5 zero-observing destroy under the caller's section ───
//
// The borrow miss/eviction path runs `destroy<Node>` inside the CALLER's
// section — which may be arbitrarily long and may have other borrows
// outstanding. Old shape: the destroy ran inside the descent's own
// one-operation section with nothing else held. This drives the new shape:
// borrow b2 (live, outstanding), then borrow a VA whose cache entry pins a
// node that a MAP_FIXED replacement detached — the hit observes the mark,
// evicts, releases the LAST count and destroys the node right there, inside
// the same section b2 is relying on. b2 must be unaffected, and the detached
// borrow must return the REPLACEMENT record, not the stale answer.
TEST(radix_borrow_detached_evict_destroy_runs_under_the_callers_section) {
    BareArena arena;
    Space     space;
    auto      cache = std::make_unique<CacheB>();

    const uint64_t region = kInteriorSpan;               // one whole level-5 subtree
    const uint64_t va     = region + kLeafNodeSpan;      // inside the subtree
    const uint64_t va2    = 8 * kInteriorSpan;           // a different subtree
    (void)space.mapPair(va);
    (void)space.mapPair(va + 4 * kLeafNodeSpan);         // give the detach a real subtree
    rdx::Mapping* m2 = space.mapPair(va2);
    const uint64_t tag2 = m2->baseVA;

    fill(*cache, *space.block, va);
    ASSERT_TRUE(stat(cache->stats().installs) == 1);

    // MAP_FIXED over the whole subtree: every node beneath is detached and
    // marked (invariant 23), the tree's own counts released. Drive the grace
    // period so the CACHE's pin is the last count standing.
    rdx::Mapping* replacement = makeMapping(region);
    ASSERT_TRUE(replacement != nullptr);
    space.map(region, region + region - 1, replacement);
    // `barrier`, not a tryAdvance loop: the detach's node retirees sit in this
    // CPU's own OPEN bag, which no sweep can claim until it is sealed — and
    // sealing happens on rotation, which barrier forces for its own caller.
    // After this, every node deleter has run and the cache's pin is the last
    // count on the detached-but-pinned node.
    kernel::rcu::barrier(space.block->domain);

    {
        rdx::BorrowWindow window(space.block->domain);

        // The outstanding borrow the destroy must not disturb.
        auto b2 = cache->borrow(*space.block, va2);
        ASSERT_TRUE(static_cast<bool>(b2) && b2.mapping() == m2);

        // The detached hit: mark observed, entry evicted, last count released,
        // destroy<Node> executed — all inside THIS window's section, with b2
        // live. The fall-through descent must answer with the replacement.
        auto b = cache->borrow(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(b));
        ASSERT_TRUE(b.mapping() == replacement);
        ASSERT_TRUE(stat(cache->stats().detachedEvictions) == 1);
        ASSERT_TRUE(stat(cache->stats().releasesThatDestroyed) == 1);

        // b2 is still exactly what it was.
        ASSERT_TRUE(b2.mapping()->baseVA == tag2);
    }

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── Attack 4: the survives-unmap property, tested NON-vacuously ────────────
//
// The branch's own survives-unmap test has its writer pump with the target's
// retirees still in the writer's OPEN bag — which no sweep can ever claim, so
// the record survives whether or not the reader's section protects anything.
// (Demonstrated by attack 3's first version: with NO section held anywhere,
// the identical writer sequence also failed to destroy the record.)
//
// This version makes the record's destruction REACHABLE up to exactly the
// reader's section: the writer performs a second unmap, whose prepareOpenBag
// rotation SEALS the bag holding the target's retirees. From that moment the
// only thing between the sealed, drain-eligible bag and destroy<Mapping> is
// the epoch gate — and the only thing blocking the epoch is the borrower's
// open section. If the borrow contract's claim 1 is wrong anywhere in the
// engine, the writer's pump loop destroys the record and the reader's
// dereference below is an ASan use-after-poison halt.
TEST(radix_borrowed_mapping_survives_even_when_its_bag_is_sealed_and_drain_eligible) {
    BareArena arena(2);
    Space     space(2);
    auto      cache = std::make_unique<CacheB>();

    const uint64_t va  = 4 * kLeafNodeSpan;
    const uint64_t vaB = 32 * kLeafNodeSpan;     // the bag-sealing sacrifice
    rdx::Mapping* m = space.mapPair(va);
    (void)space.mapPair(vaB);
    const uint64_t backendTag = m->baseVA;
    fill(*cache, *space.block, va);

    std::atomic<bool> borrowed{false};
    std::atomic<bool> unmapped{false};

    std::thread writer([&] {
        kernel::test::bindThreadToCpu(1);
        while (!borrowed.load(std::memory_order_acquire)) {}
        space.unmap(va, va + 2 * kPage - 1);
        (void)kernel::rcu::tryAdvance(space.block->domain);
        // The second unmap's prepareOpenBag sees the epoch past the first
        // bag's tag and rotates: the target's retirees are now SEALED —
        // claimable by any CPU's sweep the moment the epoch reaches tag+2.
        space.unmap(vaB, vaB + 2 * kPage - 1);
        // Drive as hard as possible. Every advance past (reader snapshot + 1)
        // must fail against the reader's active slot; if one ever succeeds,
        // the sweep below it claims the sealed bag and destroys the record
        // the reader is still holding.
        for (int i = 0; i < 64; i++) {
            (void)kernel::rcu::tryAdvance(space.block->domain);
            (void)kernel::rcu::drain(space.block->domain);
        }
        unmapped.store(true, std::memory_order_release);
    });

    {
        rdx::BorrowWindow window(space.block->domain);
        auto b = cache->borrow(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(b) && b.mapping() == m);
        borrowed.store(true, std::memory_order_release);
        while (!unmapped.load(std::memory_order_acquire)) {}
        // Sealed, expiry-eligible-but-for-us, hammered 64 times: if the
        // section's protection has a hole, this read is the crash.
        ASSERT_TRUE(b.mapping()->baseVA == backendTag);
        ASSERT_TRUE(liveCountOf<rdx::Mapping>() >= 1);
    }   // window closes; the pump may now legally destroy the record

    writer.join();
    kernel::test::bindThreadToCpu(0);

    space.quiesceSpace();
    ASSERT_TRUE(liveCountOf<rdx::Mapping>() == 0);

    cache->evictAllOnThisCpu();
    space.quiesceSpace();
}

// ─── Attack 3 (opt-in): carrying a borrow across its window's close ─────────
//
// The misuse claim 5 concedes is unpreventable: BorrowedLookup is trivially
// copyable, so nothing stops a caller saving one, closing the window, and
// dereferencing it after the grace period completes. This drives that to the
// crash it is: after the close, a concurrent unmap's reclamation is free to
// finish, and the DEC-052 oracle poisons the record on destroy, so the late
// dereference is an ASan use-after-poison halt. Opt-in via
// CROCOS_BORROW_UAF_DEMO=1 because SUCCEEDING means crashing the runner —
// this exists as evidence the contract edge is real and visible to the
// instrument, not as a gate.
TEST(radix_borrow_use_after_window_close_is_a_uaf_demo) {
    if (const char* on = std::getenv("CROCOS_BORROW_UAF_DEMO"); on == nullptr || on[0] != '1') {
        std::printf("\n  SKIP borrow UAF demo — set CROCOS_BORROW_UAF_DEMO=1 to run it "
                    "(it deliberately crashes under ASan)\n");
        return;
    }

    BareArena arena(2);
    Space     space(2);
    auto      cache = std::make_unique<CacheB>();

    const uint64_t va = 4 * kLeafNodeSpan;
    rdx::Mapping* m = space.mapPair(va);
    fill(*cache, *space.block, va);

    // A second, unrelated mapping whose later unmap forces the writer's open
    // bag to ROTATE AND SEAL. Without it the target's retirees sit in the
    // writer's own Open bag forever — unclaimable by any sweep — and the late
    // dereference reads live memory for a reason that has nothing to do with
    // the borrow contract. (That discovery is itself a finding about the
    // branch's survives-unmap test; see the strengthened test below.)
    const uint64_t vaB = 32 * kLeafNodeSpan;
    (void)space.mapPair(vaB);

    rdx::BorrowedLookup saved;
    {
        rdx::BorrowWindow window(space.block->domain);
        saved = cache->borrow(*space.block, va);
        ASSERT_TRUE(static_cast<bool>(saved) && saved.mapping() == m);
    }   // window closes: the borrow's protection is GONE

    // Unmap and drive reclamation to completion from another CPU. The second
    // unmap is what seals the first one's bag (prepareOpenBag's rotation fires
    // once the epoch has passed the bag's tag); the pump loop then claims and
    // drains it, destroying and poisoning the target record.
    std::thread writer([&] {
        kernel::test::bindThreadToCpu(1);
        space.unmap(va, va + 2 * kPage - 1);
        (void)kernel::rcu::tryAdvance(space.block->domain);
        space.unmap(vaB, vaB + 2 * kPage - 1);   // rotates + seals the first bag
        for (int i = 0; i < 64; i++) {
            (void)kernel::rcu::tryAdvance(space.block->domain);
            (void)kernel::rcu::drain(space.block->domain);
        }
    });
    writer.join();
    kernel::test::bindThreadToCpu(0);
    std::printf("  UAF demo: live Mapping count after reclamation drive: %zu\n",
                liveCountOf<rdx::Mapping>());

    // The record is destroyed and poisoned. This dereference is the §7.3
    // hazard verbatim — expected outcome: ASan use-after-poison halt.
    std::printf("  UAF demo: dereferencing a borrow after its window closed...\n");
    volatile uint64_t sink = saved.mapping()->baseVA;
    (void)sink;
    throw AssertionFailure("borrow UAF demo: the late dereference was NOT caught — "
                           "the oracle failed to poison, or reclamation never ran");
}
