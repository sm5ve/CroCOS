//
// radix-tree Phase 3 — the control block, creation and teardown
// (§7.4; DEC-082 / DEC-101).
//
// DEC-101 is a SEQUENCE with an explicit reverse unwind, and the phase plan is
// blunt about §7.4: **every reordering reviewed was a fatal**. So the tests are
// shaped around the orderings rather than around "creating an address space
// works", and the sharpest of them is the unwind:
//
//   "Any null mid-sequence unwinds in reverse — free the pools drawn so far,
//    free the root page, **`deinit()`** ... return the control block to its
//    freelist — and surfaces `ENOMEM`."
//
// The `deinit()` is the step a reader forgets, and forgetting it "leaks the
// pinned reservation permanently and silently, and breaks vmsmalloc DEC-050's
// high-water-mark claim, since the reservation would then grow with cumulative
// process churn". Nothing observable goes wrong at the time. The sweep below
// fails creation at every allocation index in turn and requires the accounting
// to come back to baseline each time — which is the only way that step's absence
// is visible.
//
// The other thing worth stating: **a recycled block is the caller's to re-zero**
// (vmsmalloc DEC-051). Zero-fill is a per-RESERVATION guarantee, so a block
// coming off the freelist carries its previous tenant's `dying` flag. A stale
// one means the next address space is born dead — silent, durable, and
// impossible to attribute later.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/AddressSpace.h>
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>

#include <cstdio>
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
using ValidA    = TreeValidator<GA, CodecA>;

constexpr uint64_t kPage = 4096;

// The real ceiling, not a convenient small number. `CoreTree::bindToBucket`
// asserts the pool is at least this deep — correctly, since a shallower one
// turns every operation into a shortfall-and-barrier round trip — so a test that
// wanted a cheaper sweep would have to weaken a load-bearing assert to get it.
// The sweep is ~230 creations of ~230 allocations each and runs in well under a
// second, so there was nothing to buy.
constexpr unsigned kTestRecords = rdx::deferredReleaseBound(GA);

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// The bare arena, without RadixHarness's own domain and pools — an address space
// creates those itself, and having the fixture create a second set would make
// every accounting assertion below ambiguous.
struct BareArena {
    explicit BareArena(size_t cpus = 1) {
        VS::test::initialize(cpus, 1);
        numa::test::configure(cpus, 1, &allToDomainZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        kernel::mm::radix::HarnessSlotBase::bind(
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

}  // namespace

// ─── The block anchors what §7.4 says it anchors ───────────────────────────

TEST(radix_address_space_creation_publishes_a_usable_block) {
    BareArena arena;
    FreelistA freelist;
    BlockA* block = nullptr;

    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);
    ASSERT_TRUE(block != nullptr);

    // Everything §7.4 names as anchored here, live and usable.
    ASSERT_TRUE(block->domain.initialized());
    ASSERT_TRUE(block->clusters.valid());
    ASSERT_TRUE(block->pools.perCpu == kTestRecords);
    ASSERT_TRUE(!rdx::isDying(*block));
    ASSERT_TRUE(block->generation != 0);

    // ...and the address space actually works.
    ASSERT_TRUE(block->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = block->clusters.treeFor(0);
    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(static_cast<bool>(t.lookup(0)));

    rdx::destroyAddressSpace(freelist, block);
    assertNoLiveObjects("creation and teardown");
}

// ─── Teardown returns everything ───────────────────────────────────────────

TEST(radix_address_space_teardown_returns_to_baseline) {
    BareArena arena;
    FreelistA freelist;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    // Populate several clusters, including one that grows, and then churn — so
    // the teardown has nodes, mappings, an outstanding DeferredRelease or two,
    // and more than one bucket to walk.
    const uint64_t zone = rdx::slotSpan(GA, 0);
    for (uint64_t b = 0; b < 3; b++) {
        const uint64_t va = b * zone + b * kPage;
        ASSERT_TRUE(block->clusters.ensureCovers(va, va + kPage - 1)
                    == rdx::ClusterStatus::Ok);
        TreeA t = block->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
        for (unsigned k = 0; k < 4; k++) {
            auto* m = makeMapping(va);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);
        }
    }

    // Grow one of them, so teardown walks a multi-level cluster.
    ASSERT_TRUE(block->clusters.growToCover(0, rdx::nodeSpan(GA, GA.defaultRootLevel))
                == rdx::ClusterStatus::Ok);

    rdx::destroyAddressSpace(freelist, block);

    // Nodes, Mappings, DeferredRelease records, the bucket page — all of it. The
    // record pools in particular are NOT retire subjects, so a literal "retire
    // everything" teardown leaks them and this is where that shows.
    assertNoLiveObjects("teardown");
}

// ─── The unwind, at every step (DEC-101) ───────────────────────────────────

// Phase 0 built the injection hooks for exactly this. The sweep fails the n'th
// allocation of the creation sequence, for every n the sequence reaches, and
// requires each unwind to leave the accounting at baseline.
//
// A creation that forgot `deinit()` passes every functional check here — the
// call returns ENOMEM, nothing crashes, the next creation works — and fails only
// because the domain's pinned block never came back. That is the whole point of
// sweeping rather than testing one failure.
TEST(radix_address_space_creation_unwinds_at_every_step) {
    BareArena arena;
    FreelistA freelist;

    // How many allocations a full creation makes, measured rather than assumed:
    // the sequence's shape is what is under test, so hard-coding the count would
    // make the sweep stop tracking it.
    size_t fullCount = 0;
    {
        resetInjection();
        injectFailuresAfter(1u << 30);   // effectively never
        BlockA* block = nullptr;
        ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                            kTestRecords, block)
                    == rdx::CreateStatus::Ok);
        fullCount = injectionObservedCalls();
        rdx::destroyAddressSpace(freelist, block);
        resetInjection();
        assertNoLiveObjects("sweep baseline");
    }
    ASSERT_TRUE(fullCount > 1);

    for (size_t n = 0; n < fullCount; n++) {
        resetInjection();
        injectFailureAt(n);

        BlockA* block = nullptr;
        const auto st = rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                                kTestRecords, block);
        resetInjection();

        if (st == rdx::CreateStatus::Ok) {
            // The injected failure landed somewhere the sequence tolerates —
            // there is no such site today, but a future step with a retry would
            // make this reachable, and silently skipping it would hide it.
            ASSERT_TRUE(block != nullptr);
            rdx::destroyAddressSpace(freelist, block);
        } else {
            // The contract: nothing published, and nothing left behind.
            ASSERT_EQ(nullptr, block);
        }
        char what[64];
        std::snprintf(what, sizeof(what), "unwind at allocation %zu", n);
        assertNoLiveObjects(what);
    }
}

// The reservation itself failing is a different path — it happens before any
// allocation, under the domain-management lock, and its unwind is "return
// nothing, because nothing was taken".
TEST(radix_address_space_creation_handles_a_failed_reservation) {
    BareArena arena;
    FreelistA freelist;

    VS::test::setStaticReservationFailAt(0);
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::OutOfMemory);
    ASSERT_EQ(nullptr, block);
    VS::test::setStaticReservationFailAt(-1);

    assertNoLiveObjects("failed reservation");

    // ...and the path recovers: the next creation succeeds.
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);
    rdx::destroyAddressSpace(freelist, block);
    assertNoLiveObjects("recovery");
}

// ─── Recycling: the block comes back, and it comes back CLEAN ──────────────

TEST(radix_address_space_blocks_recycle_through_the_freelist) {
    BareArena arena;
    FreelistA freelist;

    BlockA* first = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, first)
                == rdx::CreateStatus::Ok);
    const uint64_t gen1 = first->generation;

    // Mark it dying and tear it down, so the block goes back to the freelist
    // carrying a set flag — which is precisely the state a recycled block must
    // not inherit.
    rdx::destroyAddressSpace(freelist, first);

    BlockA* second = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, second)
                == rdx::CreateStatus::Ok);

    // The same pinned storage: reservations are kernel-lifetime and never
    // returned to VMSubstrate, which is what keeps the high-water mark at
    // "maximum concurrent address spaces" rather than growing with churn.
    ASSERT_EQ(first, second);

    // Re-zeroed. A stale `dying` would make this address space born dead:
    // every operation would take the teardown path, silently, forever.
    ASSERT_TRUE(!rdx::isDying(*second));
    // ...and the generation MOVED, which is what lets Phase 4's descent cache
    // tell a recycled block's entries from a live one's.
    ASSERT_TRUE(second->generation != gen1);

    // It works.
    ASSERT_TRUE(second->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = second->clusters.treeFor(0);
    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(static_cast<bool>(t.lookup(0)));

    rdx::destroyAddressSpace(freelist, second);
    assertNoLiveObjects("recycling");
}

// ─── The dying flag ────────────────────────────────────────────────────────

TEST(radix_address_space_dying_flag_is_set_before_the_barrier) {
    BareArena arena;
    FreelistA freelist;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    ASSERT_TRUE(!rdx::isDying(*block));
    // Set it by hand and observe it through the acquire side — the ordering pair
    // is named in §6.6 precisely because the flag alone establishes nothing and
    // a bare spelling would invite someone to trust it on its own.
    block->dying.store(1, rdx::kDyingFlagStore);
    ASSERT_TRUE(rdx::isDying(*block));
    block->dying.store(0, rdx::kPrivateInit);

    rdx::destroyAddressSpace(freelist, block);
    assertNoLiveObjects("dying flag");
}

// ─── DEC-100's unit-decomposed walk ────────────────────────────────────────

// The three properties that distinguish it from a synchronous post-order
// release, driven by running §7.4's steps by hand so the window between the walk
// and the drain is observable at all.
//
// The one that matters most for Phase 4 is the **marking**. §7.4: "Without any
// marking at all, teardown would be the only unlink path setting no mark,
// leaving a foreign CPU's surviving cache entry pointed at a node that is
// unlinked, unmarked, alive, and holding slots that point at freed children."
// There is no descent cache yet, so nothing observes it today — which is exactly
// why it needs a test now rather than when the cache lands and the symptom is a
// silent wrong mapping.
TEST(radix_teardown_walk_marks_and_retires_rather_than_destroying) {
    BareArena arena;
    FreelistA freelist;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(freelist, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    ASSERT_TRUE(block->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = block->clusters.treeFor(0);
    // Two disjoint sub-ranges, so the cluster is a root plus a real child and
    // the walk has more than one unit to run.
    auto* a = makeMapping(0);
    auto* b = makeMapping(4 * kPage);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(t.apply(4 * kPage, 5 * kPage - 1, b) == rdx::ApplyStatus::Ok);
    (void)kernel::rcu::drainAllQuiescent(block->domain);

    const size_t nodesBefore = t.nodeCount();
    ASSERT_TRUE(nodesBefore > 1);
    rdx::NodeRef root = t.root();

    // Capturing a node pointer across the walk is legal for the test for the
    // same reason the walk itself carries one across its children's sections:
    // by here nothing else is running.
    ASSERT_TRUE(!rdx::state::isMarked(root.stateWord().load(RELAXED)));

    // §7.4's steps, by hand, so the window is observable.
    block->dying.store(1, rdx::kDyingFlagStore);
    kernel::rcu::synchronize(block->domain);

    const size_t liveNodesBeforeWalk = liveCountOf<rdx::Node<GA, 32>>()
                                     + liveCountOf<rdx::Node<GA, 16>>();
    block->clusters.tearDownClusters();

    // 1. MARKED. The property with no symptom until Phase 4.
    ASSERT_TRUE(rdx::state::isMarked(root.stateWord().load(RELAXED)));

    // 2. RETIRED, not destroyed: the nodes are still live objects between the
    //    walk and the drain. A synchronous walk destroys them in step, and this
    //    count would already be zero.
    ASSERT_EQ(liveNodesBeforeWalk,
              liveCountOf<rdx::Node<GA, 32>>() + liveCountOf<rdx::Node<GA, 16>>());

    // 3. The bucket word is cleared by the walk's final unit, inside its own
    //    section — so the cluster is unreachable even though its nodes are not
    //    yet destroyed.
    ASSERT_TRUE(!block->clusters.bucketIsOccupied(0));

    // ...and the drain is what actually destroys them.
    (void)kernel::rcu::drainAllQuiescent(block->domain);
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::Node<GA, 32>>() + liveCountOf<rdx::Node<GA, 16>>());

    block->clusters.freeRootPage();
    block->pools.destroy();
    (void)block->domain.deinit();
    {
        kernel::rcu::DomainManagementLockGuard guard;
        freelist.returnLocked(block);
    }
    assertNoLiveObjects("unit-decomposed walk");
}
