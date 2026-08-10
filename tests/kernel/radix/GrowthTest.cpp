//
// radix-tree Phase 3 — cluster creation and growth (§5.6; DEC-026/027/028/067/
// 090/103).
//
// §5.6's growth paragraph is the newest text in the spec and the phase plan
// flags every reordering of it as a fatal, so these tests are written against
// its clauses one at a time rather than against "growth works". Three of them
// have no observable symptom if they are wrong:
//
//   - **No count moves on the old root.** The bucket word's inbound reference
//     transfers *in place* to the new parent's child slot. A grower that took a
//     `+1` here would leave the old root pinned for the address space's life —
//     invisible until a teardown residue check, and a `-1` would be a
//     use-after-free that only fires under a concurrent reader.
//   - **Nothing is retired.** The old bucket word is a value, not an object.
//   - **A losing grower shallow-discards ONE node.** Its private parent already
//     names the still-live old root in a child slot, so a recursive discard
//     takes the whole cluster with it — a plausible-looking edit with a
//     catastrophic radius.
//
// The exit gate names the **lost-CAS row** specifically, and that is the last
// one: it is only reachable under genuine concurrency, and it is where the
// pre-assigned count of 1 and the shallow discard have to agree.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA   = rdx::HarnessSlotCodec<GA>;
using BucketA  = rdx::HarnessBucketCodec<GA>;
using TableA   = rdx::ClusterTable<GA, CodecA>;
using TreeA    = rdx::CoreTree<GA, CodecA>;
using ValidA   = TreeValidator<GA, CodecA>;

constexpr uint64_t kPage = 4096;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// A VA in bucket 2, well inside its zone, so nothing under test is hiding
// behind a zero base.
const uint64_t kZone = rdx::slotSpan(GA, 0);
const uint64_t kVA   = 2 * kZone + 5 * rdx::nodeSpan(GA, GA.defaultRootLevel);

struct Fixture {
    Harness h;
    TableA  table;

    explicit Fixture(size_t cpus = 1) : h(cpus, 1) {
        // The pool contract is "caller holds the domain-management lock"; a
        // single-threaded fixture takes it for form rather than for safety, so
        // the call shape here matches the kernel's.
        kernel::rcu::DomainManagementLockGuard guard;
        if (!table.initLocked(h.domain, h.releasePools, h.rootPages,
                              kernel::numa::DomainID{0})) {
            throw AssertionFailure(std::string("cluster table init failed"));
        }
    }
    // ─── Why teardown is a METHOD and not only a destructor (R-5) ───────────
    //
    // The allocation oracle is the only detector in this suite that can see a node
    // that was allocated and never published: `nodeCount()` and `TreeValidator`
    // both walk the PUBLISHED tree, and LSan sees nothing because the whole arena
    // is one live mmap that the fixture unmaps wholesale.
    //
    // Growth and creation both allocate a private node, try to publish it with a
    // CAS, and **shallow-discard it on the losing side** (`ClusterTable.h:229` and
    // `:309`). A loser that forgot to discard is therefore invisible to every
    // check these tests were making — verified by mutation: deleting the discard
    // left the suite at 173/173 green.
    //
    // The oracle assertion has to run AFTER the table is torn down, and a
    // destructor runs after the test body, so it cannot be there. Hence this: tear
    // down explicitly, then assert. `finished` keeps the destructor from doing it
    // twice, since `ClusterTable::destroy` is not idempotent — and it is set
    // BEFORE the assertion, so a failure does not turn into a double free.
    void finishAndAssertNoLeaks(const char* what) {
        table.destroy();
        quiesce(h);
        finished = true;
        assertNoLiveObjects(what);
    }

    ~Fixture() {
        if (finished) return;
        table.destroy();
        quiesce(h);
    }

    bool finished = false;
};

}  // namespace

// ─── DEC-090: the root level a request needs ───────────────────────────────

TEST(radix_growth_root_level_covers_the_request) {
    // Anything fitting the default cluster gets the default root — a smaller
    // request never gets a DEEPER one, because depth is what the default level
    // is chosen for.
    ASSERT_EQ(GA.defaultRootLevel, rdx::rootLevelCovering<GA>(kPage));
    ASSERT_EQ(GA.defaultRootLevel,
              rdx::rootLevelCovering<GA>(rdx::nodeSpan(GA, GA.defaultRootLevel)));

    // Oversized requests root at the smallest level whose node span covers
    // them. Necessity, not adaptivity: the mapping must fit its cluster.
    for (unsigned level = GA.defaultRootLevel; level >= 1; level--) {
        const uint64_t span = rdx::nodeSpan(GA, level);
        ASSERT_EQ(level, rdx::rootLevelCovering<GA>(span));
        if (level > 1) ASSERT_EQ(level - 1, rdx::rootLevelCovering<GA>(span + 1));
    }
    // DEC-080 bounds it above at the bucket: level 1's node span IS the zone.
    ASSERT_EQ(unsigned{1}, rdx::rootLevelCovering<GA>(kZone));
}

// ─── Creation publishes an EMPTY root, and placing is a separate step ──────

TEST(radix_cluster_creation_publishes_an_empty_root) {
    Fixture f;
    const size_t idx = rdx::bucketIndexFor<GA>(kVA);

    ASSERT_TRUE(!f.table.bucketIsOccupied(idx));
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);
    ASSERT_TRUE(f.table.bucketIsOccupied(idx));

    TreeA t = f.table.treeFor(idx);
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(GA.defaultRootLevel, t.rootLevel());
    ASSERT_EQ(size_t{1}, t.nodeCount());          // the root, and nothing else
    ASSERT_TRUE(!static_cast<bool>(t.lookup(kVA)));  // EMPTY — this is the point

    // ...and placing into it afterwards is an ordinary claimed-slot operation.
    auto* m = makeMapping(kVA);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(kVA, kVA + kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(static_cast<bool>(t.lookup(kVA)));
    quiesce(f.h);
    ValidA::validate(t, "creation");

    f.finishAndAssertNoLeaks("cluster creation");
}

TEST(radix_cluster_creation_is_idempotent_per_bucket) {
    Fixture f;
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);
    // DEC-033: at most one cluster per bucket. A second creator adopts the
    // first's cluster rather than minting a second — and gets told so.
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel)
                == rdx::ClusterStatus::AlreadyPresent);
    ASSERT_TRUE(f.table.createCluster(kVA + kPage, GA.defaultRootLevel)
                == rdx::ClusterStatus::AlreadyPresent);

    TreeA t = f.table.treeFor(rdx::bucketIndexFor<GA>(kVA));
    ASSERT_EQ(size_t{1}, t.nodeCount());

    f.finishAndAssertNoLeaks("creation idempotence");
}

// ─── Growth: one level at a time, and it only ever widens ──────────────────

TEST(radix_growth_widens_the_cluster_to_cover_the_range) {
    Fixture f;
    const size_t idx = rdx::bucketIndexFor<GA>(kVA);
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);

    TreeA t = f.table.treeFor(idx);
    const uint64_t base0  = t.base();
    const uint64_t span0  = t.span();
    const unsigned level0 = t.rootLevel();

    // A target one span above the cluster: not covered, so growth is forced.
    const uint64_t target = base0 + span0;
    ASSERT_TRUE(!t.contains(target));

    ASSERT_TRUE(f.table.growToCover(kVA, target) == rdx::ClusterStatus::Ok);

    ASSERT_TRUE(t.rootLevel() < level0);   // rooted higher
    ASSERT_TRUE(t.span() > span0);         // over a wider span
    ASSERT_TRUE(t.base() <= base0);        // whose base can only move down
    ASSERT_TRUE(t.contains(kVA) && t.contains(target));

    // Idempotent: a range the cluster already covers is not a growth.
    const unsigned grown = t.rootLevel();
    ASSERT_TRUE(f.table.growToCover(kVA, target) == rdx::ClusterStatus::Ok);
    ASSERT_EQ(grown, t.rootLevel());

    f.finishAndAssertNoLeaks("growth widens");
}

// The clause with no symptom: the old root is now a child of the new one, and
// **its count never moved**. A `+1` here pins it for the address space's life;
// a `-1` is a use-after-free under a concurrent reader. Neither shows up in any
// coverage or shape check, which is why this is asserted directly.
TEST(radix_growth_moves_no_count_and_retires_nothing) {
    Fixture f;
    const size_t idx = rdx::bucketIndexFor<GA>(kVA);
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);

    TreeA t = f.table.treeFor(idx);
    // Put a mapping in, so the old root is not a bare node and the walk below
    // has something to disagree about if references moved.
    auto* m = makeMapping(kVA);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(kVA, kVA + kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(f.h);

    rdx::NodeRef oldRoot = t.root();
    const uint64_t countBefore = oldRoot.refcount().load(RELAXED);
    const size_t   nodesBefore = t.nodeCount();
    const uint64_t mappingBefore = m->refcountRelaxed();

    ASSERT_TRUE(f.table.growToCover(kVA, t.base() + t.span()) == rdx::ClusterStatus::Ok);
    quiesce(f.h);

    // Transfer in place: the bucket's reference became the parent's child-slot
    // reference, and the count is untouched on both sides of that.
    ASSERT_EQ(countBefore, oldRoot.refcount().load(RELAXED));
    // The new root carries its pre-assigned 1 and nothing more.
    ASSERT_EQ(uint64_t{1}, t.root().refcount().load(RELAXED));
    ASSERT_TRUE(t.root() != oldRoot);
    // Exactly one node was added, and nothing was retired or freed: a retire
    // would have shown up here as a node vanishing after the quiesce.
    ASSERT_EQ(nodesBefore + 1, t.nodeCount());
    // No leaf slot was created or destroyed, so no Mapping count moved either.
    ASSERT_EQ(mappingBefore, m->refcountRelaxed());

    ValidA::validate(t, "growth accounting");
    ASSERT_TRUE(static_cast<bool>(t.lookup(kVA)));   // and nothing moved

    f.finishAndAssertNoLeaks("growth accounting");
}

// Growth is repeatable up to the zone, which is where DEC-080 takes over.
TEST(radix_growth_walks_up_to_the_zone_and_stops) {
    Fixture f;
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);
    TreeA t = f.table.treeFor(rdx::bucketIndexFor<GA>(kVA));

    const uint64_t zoneBase = rdx::bucketIndexFor<GA>(kVA) * kZone;
    ASSERT_TRUE(f.table.growToCover(zoneBase, zoneBase + kZone - 1)
                == rdx::ClusterStatus::Ok);

    // Rooted at level 1, whose node span IS the zone — the top of the bucket.
    ASSERT_EQ(unsigned{1}, t.rootLevel());
    ASSERT_EQ(zoneBase, t.base());
    ASSERT_EQ(kZone, t.span());
    // One node per level from the new root down to the original: growing from
    // the default root level to level 1 ADDS nodes, it does not replace them.
    ASSERT_EQ(static_cast<size_t>(GA.defaultRootLevel), t.nodeCount());

    f.finishAndAssertNoLeaks("growth to the zone");
}

// ─── ensureCovers: the shape a fixed-address placement wants ───────────────

TEST(radix_ensure_covers_creates_then_grows) {
    Fixture f;
    const uint64_t lo = kVA;
    const uint64_t hi = kVA + 4 * rdx::nodeSpan(GA, GA.defaultRootLevel) - 1;

    ASSERT_TRUE(f.table.ensureCovers(lo, hi) == rdx::ClusterStatus::Ok);

    TreeA t = f.table.treeFor(rdx::bucketIndexFor<GA>(lo));
    ASSERT_TRUE(t.contains(lo) && t.contains(hi));

    auto* m = makeMapping(lo);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(lo, hi, m) == rdx::ApplyStatus::Ok);
    quiesce(f.h);
    ValidA::validate(t, "ensureCovers");
    ASSERT_TRUE(static_cast<bool>(t.lookup(lo)));
    ASSERT_TRUE(static_cast<bool>(t.lookup(hi)));

    f.finishAndAssertNoLeaks("ensureCovers");
}

// A request too large for the default cluster is rooted high at creation
// (DEC-090) rather than created small and immediately grown.
TEST(radix_ensure_covers_roots_an_oversized_request_high) {
    Fixture f;
    const uint64_t clusterSpan = rdx::nodeSpan(GA, GA.defaultRootLevel);
    const uint64_t lo = rdx::bucketIndexFor<GA>(kVA) * kZone;
    const uint64_t hi = lo + 3 * clusterSpan - 1;   // three default clusters wide

    ASSERT_TRUE(f.table.ensureCovers(lo, hi) == rdx::ClusterStatus::Ok);
    TreeA t = f.table.treeFor(rdx::bucketIndexFor<GA>(lo));
    ASSERT_TRUE(t.rootLevel() < GA.defaultRootLevel);
    ASSERT_TRUE(t.contains(lo) && t.contains(hi));
    // Rooted directly at the covering level, so no growth was needed and the
    // cluster is a single node.
    ASSERT_EQ(size_t{1}, t.nodeCount());

    f.finishAndAssertNoLeaks("oversized request");
}

// ─── The exit gate's lost-CAS row ──────────────────────────────────────────

// Only reachable under genuine concurrency, and the place where the pre-assigned
// count of 1 and the shallow discard have to agree. Every CPU races to create in
// the same bucket and then to grow it; exactly one creation wins, every loser
// discards its private root, and the survivors' accounting must be as clean as
// the single-threaded case.
//
// The leak/double-free half is carried by the oracle: a loser that FORGOT to
// discard shows up as a live node at the end, and one that discarded
// RECURSIVELY takes the winner's cluster with it and fails long before that.
//
// **That sentence was true of the intent and false of the code (R-5).** The test
// never called the oracle. What it actually checked was the validator, a `>=`
// bound on `nodeCount()` and two refcounts — none of which can see an unpublished
// allocation, and a `>=` bound cannot see an extra published one either. Deleting
// the discard at `ClusterTable.h:309` left the whole suite green.
//
// Both halves are now asserted, and the test races BOTH discard sites of this
// class: every CPU's `createCluster` loses (the cluster is seeded, so all four
// adopt and each discards a private root at `:229`), and then their growths race
// (`:309`). Four CPUs times two sites is what makes one leaked node visible.
TEST(radix_growth_lost_cas_leaks_nothing_and_corrupts_nothing) {
    constexpr size_t kCpus = 4;
    // Rounds beyond the asserted first one, purely to make the leak detector
    // reliable rather than probabilistic — see the note at the loop.
    constexpr size_t kExtraRounds = 24;
    Fixture f(kCpus);
    const size_t idx = rdx::bucketIndexFor<GA>(kVA);

    std::atomic<size_t> created{0};
    std::atomic<size_t> adopted{0};
    std::atomic<size_t> failures{0};
    std::string firstError;
    std::mutex errorMutex;
    std::atomic<bool> go{false};

    // Place one mapping before the race. Not decoration: growing an EMPTY
    // cluster leaves the old root as an empty non-root node that nothing will
    // ever reclaim (D-030), and this test is about the CAS, not about that.
    {
        TreeA t = f.table.treeFor(idx);
        ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);
        auto* seed = makeMapping(kVA);
        ASSERT_TRUE(seed != nullptr);
        ASSERT_TRUE(t.apply(kVA, kVA + kPage - 1, seed) == rdx::ApplyStatus::Ok);
        quiesce(f.h);
    }

    // Measured before the race, so the exact post-race count below is derived
    // rather than transcribed: growth ADDS one node per level walked and changes
    // nothing else (which `radix_growth_walks_up_to_the_zone_and_stops`
    // establishes), so whatever the seed's placement built stays put.
    const size_t nodesBeforeRace = f.table.treeFor(idx).nodeCount();

    // A rendezvous per round, so every CPU enters the growth climb at the same
    // instant. Bounded, so a worker that throws cannot wedge the others — the
    // waiter reports a fixture failure instead, which is the honest outcome: a run
    // where one CPU died proves nothing about the discard.
    std::atomic<size_t> arrived{0};
    auto roundBarrier = [&](size_t round) {
        const size_t target = kCpus * round;
        arrived.fetch_add(1, std::memory_order_acq_rel);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (arrived.load(std::memory_order_acquire) < target) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    };

    std::vector<std::thread> workers;
    for (size_t c = 0; c < kCpus; c++) {
        workers.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            while (!go.load(std::memory_order_acquire)) { }
            try {
                const auto st = f.table.createCluster(kVA, GA.defaultRootLevel);
                if (st == rdx::ClusterStatus::Ok) created.fetch_add(1, std::memory_order_relaxed);
                else if (st == rdx::ClusterStatus::AlreadyPresent)
                    adopted.fetch_add(1, std::memory_order_relaxed);
                else throw AssertionFailure(std::string("creation ran out of memory"));

                // Now race the growths. Each CPU asks for a different width, so
                // the losers are genuinely losers rather than no-ops.
                TreeA t = f.table.treeFor(idx);
                // Above kVA, and a different width per CPU so the losers are
                // genuinely losers rather than no-ops.
                const uint64_t want = kVA +
                    (uint64_t{1} << (c + 1)) * rdx::nodeSpan(GA, GA.defaultRootLevel) - 1;
                if (f.table.growToCover(kVA, want) != rdx::ClusterStatus::Ok) {
                    throw AssertionFailure(std::string("growth failed"));
                }

                // ─── Extra rounds, because ONE race is not a detector ───────
                //
                // The round above is the one with the exact shape assertions, and
                // on its own it is only a probabilistic detector of a missed growth
                // discard: the CAS has to actually be LOST, and the window is
                // narrow. If the widest-target CPU happens to run first it climbs
                // all the way and every other CPU's `growToCover` returns without
                // allocating anything, so there is no loser to leak. Measured with
                // the discard deleted: one round caught it in **10 of 12 runs**, and
                // 24 unsynchronised rounds only reached 11 of 12 — a coin toss
                // dressed as a test either way.
                //
                // Two changes make it reliable rather than lucky:
                //
                //   * **a barrier immediately before each growth**, so all four
                //     CPUs are inside the climb at the same instant rather than
                //     arriving whenever their creation finished — which is what
                //     was actually serialising them;
                //   * **the maximal climb** (to the whole zone, level 1), so each
                //     round offers several CAS steps to lose rather than one.
                //
                // These rounds are deliberately NOT seeded with a mapping: their
                // old roots are left behind empty by D-030, which is legitimate
                // PUBLISHED residue that teardown reclaims, so it does not disturb
                // the leak accounting. Only the first bucket needs the seed,
                // because only it has shape assertions.
                for (size_t r = 1; r <= kExtraRounds; r++) {
                    const uint64_t va       = kVA + r * kZone;
                    const uint64_t zoneBase = rdx::bucketIndexFor<GA>(va) * kZone;
                    const auto cst = f.table.createCluster(va, GA.defaultRootLevel);
                    if (cst == rdx::ClusterStatus::OutOfMemory) {
                        throw AssertionFailure(std::string("extra-round creation OOM"));
                    }
                    if (!roundBarrier(r)) {
                        throw AssertionFailure(std::string(
                            "round barrier timed out — another CPU died mid-round, so this "
                            "run proves nothing about the discard"));
                    }
                    if (f.table.growToCover(zoneBase, zoneBase + kZone - 1)
                            != rdx::ClusterStatus::Ok) {
                        throw AssertionFailure(std::string("extra-round growth failed"));
                    }
                }
            } catch (const std::exception& e) {
                failures.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) firstError = e.what();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    kernel::test::bindThreadToCpu(0);

    if (failures.load() != 0) throw AssertionFailure("worker failure: " + firstError);

    // The cluster was seeded above, so every racing creator adopts — which is
    // the same code path a loser takes and the one that must shallow-discard.
    ASSERT_EQ(size_t{0}, created.load());
    ASSERT_EQ(kCpus, adopted.load());

    quiesce(f.h);
    TreeA t = f.table.treeFor(idx);
    ValidA::validate(t, "lost CAS");

    // No loser's private parent is still LINKED, and none was destroyed while
    // linked (which would have crashed the validate above). Exact, not a `>=`
    // bound: growth adds one node per level walked, so the count is the pre-race
    // count plus the levels the winner climbed. A bound cannot see an extra node,
    // which is the whole failure mode.
    ASSERT_EQ(nodesBeforeRace + static_cast<size_t>(GA.defaultRootLevel - t.rootLevel()),
              t.nodeCount());
    ASSERT_EQ(uint64_t{1}, t.root().refcount().load(RELAXED));
    ASSERT_TRUE(static_cast<bool>(t.lookup(kVA)));   // the seed survived it all

    // **And the assertion the comment above always promised.** A loser's
    // undiscarded private node is not linked anywhere, so nothing above can see
    // it; the oracle counts allocations, so it can. Four CPUs each discarded a
    // root at `createCluster` and raced a parent at `growToCover`, so a single
    // missed discard at either site lands here.
    f.finishAndAssertNoLeaks("growth lost CAS");
}

// ─── D-030: growing an EMPTY cluster leaves an empty node behind ───────────

// Recorded rather than fixed, and pinned here so it cannot change silently.
//
// §6.4's reclamation fires when an operation EMPTIES a node. A cluster root that
// was already empty when growth pushed it below a new root is never emptied
// again — `clearsHere` is zero for it forever — so it survives until teardown.
//
// Bounded and tiny: at most one such node per growth, and growth is capped at
// `levelCount - defaultRootLevel` steps per cluster, so two nodes at the amd64
// default. Reachable in production by an address space that unmaps a cluster
// completely and then makes a fixed-address request needing a wider span.
//
// The test asserts the residue exists and is exactly one node, which is what
// makes a future fix visible: if someone teaches growth to reclaim an empty old
// root, this fails and gets deleted deliberately rather than drifting.
TEST(radix_growth_over_an_empty_cluster_leaves_the_old_root_behind) {
    Fixture f;
    ASSERT_TRUE(f.table.createCluster(kVA, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);
    TreeA t = f.table.treeFor(rdx::bucketIndexFor<GA>(kVA));
    ASSERT_EQ(size_t{1}, t.nodeCount());

    ASSERT_TRUE(f.table.growToCover(kVA, t.base() + t.span()) == rdx::ClusterStatus::Ok);
    quiesce(f.h);

    // Two nodes: the new root, and the empty old one hanging beneath it.
    ASSERT_EQ(size_t{2}, t.nodeCount());
    // It is genuinely empty, and genuinely not the root any more.
    ASSERT_TRUE(t.root() != rdx::NodeRef());
    ASSERT_TRUE(!static_cast<bool>(t.lookup(kVA)));

    // Mapping and then unmapping through it reclaims it by the ordinary path —
    // the residue is a missed opportunity, not a leak the tree cannot recover
    // from.
    auto* m = makeMapping(kVA);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(kVA, kVA + kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(f.h);
    ASSERT_TRUE(t.apply(kVA, kVA + kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(f.h);
    ASSERT_EQ(size_t{1}, t.nodeCount());   // back to the root alone
}
