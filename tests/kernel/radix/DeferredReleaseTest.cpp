//
// radix-tree Phase 3 — DEC-068's deferred `Mapping` releases (§7.1).
//
// The mechanism's *effect* — that a reader can no longer observe a record
// destroyed under it — is covered where it belongs, by the three reader-side
// tests in ConcurrentTest.cpp that were disabled for the whole of Phase 2 under
// D-011. This file covers the mechanism's own claims, which those tests would
// pass without: a per-slot record would be just as safe and would blow the
// budget, and a pool that quietly leaked one record per operation would look
// identical until it ran dry under load much later.
//
// The four claims, in the order §7.1 makes them:
//
//   1. **One record per DISTINCT `Mapping`**, carrying the aggregate decrement.
//      The typical `munmap` displaces one record from many slots, so the
//      difference between "per distinct record" and "per slot" is the difference
//      between one draw and fifteen.
//   2. **A fully-covered child draws nothing.** Its displaced values ride node
//      deleters. §7.1 is emphatic that the PARTIAL cover is the expensive shape
//      and that it is easy to invert, so this is pinned rather than assumed.
//   3. **The records recycle and are never freed.** Steady-state churn allocates
//      nothing, and every record comes home.
//   4. **A shortfall is not a failure.** It is abandoned, replenished with
//      `barrier` — never `synchronize` — and retried, and the retry succeeds
//      deterministically because only the owner draws from its own pool.
//
// Claim 4 is the one that needs a real concurrent setup, because a shortfall is
// only reachable when grace periods have stalled: the missing records are in
// flight through this CPU's own earlier retires. So a second CPU holds a read
// section open until it observes the first CPU's shortfall counter move. That
// handshake is what makes the test deterministic rather than a timing guess —
// and it is also exactly the situation §7.1 describes.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/CoreTree.h>
#include <mem/radix/DeferredRelease.h>

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
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;
using ValidA = TreeValidator<GA, CodecA>;

constexpr uint64_t kPage = 4096;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

rdx::DeferredReleasePool& poolOf(Harness& h, size_t cpu) {
    return h.releasePools.forCpu(static_cast<arch::ProcessorID>(cpu));
}

uint64_t drawsOf(Harness& h, size_t cpu) {
    return poolOf(h, cpu).draws.load(RELAXED);
}

}  // namespace

// ─── The ceiling is derived, not transcribed ───────────────────────────────

// §7.1's ≈230 is the EDGE SUM, and it is a different governing quantity from
// §6.1's site bound — which is the one already in the code and therefore the one
// an implementer is likely to reach for. The static_assert in DeferredRelease.h
// is the real gate; this restates it at runtime so a reader of the test suite
// meets the number and its derivation.
TEST(radix_deferred_release_ceiling_is_the_edge_sum) {
    ASSERT_EQ(230u, rdx::deferredReleaseBound(GA));

    // Recomputed here from the geometry the long way, so the assertion is
    // against the SPEC's formula rather than against the implementation's own
    // arithmetic restated.
    unsigned expected = rdx::valence(GA, 1);
    for (unsigned l = 2; l <= GA.levelCount; l++) expected += 2u * (rdx::valence(GA, l) - 1u);
    ASSERT_EQ(expected, rdx::deferredReleaseBound(GA));

    // And it is genuinely larger than the site bound, which is the confusion the
    // spec spends a paragraph heading off.
    ASSERT_TRUE(rdx::deferredReleaseBound(GA) > rdx::siteBound(GA));
}

// ─── Claim 1: one record per DISTINCT Mapping ──────────────────────────────

TEST(radix_deferred_release_one_record_covers_many_slots_of_one_mapping) {
    Harness h(1, 1);
    TreeA tree;
    // C3 root: 16 slots of one PAGE each. Level 5 would look right and is not —
    // a C2 node's slots span 64 KiB, so a four-page range lands in ONE of them
    // and the test would be measuring subdivisions instead of naming slots.
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));

    // One record over four slots. Each naming slot takes its own +1, so the
    // record's structural count is 4.
    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 4 * kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{4}, m->refcountRelaxed());

    const uint64_t before = drawsOf(h, 0);
    ASSERT_TRUE(tree.apply(0, 4 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    // ONE record, not four — this is the whole reason the record carries a
    // `delta` rather than a flag.
    ASSERT_EQ(uint64_t{1}, drawsOf(h, 0) - before);

    // ...and it carried the whole decrement: after the grace period the record
    // is gone, which the oracle sees as the object being destroyed.
    quiesce(h);
    ValidA::validate(tree, "one record, four slots");
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("one record, four slots");
}

TEST(radix_deferred_release_distinct_mappings_get_distinct_records) {
    Harness h(1, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));   // C3 root: page-sized slots

    // Four slots, four distinct records.
    for (uint64_t k = 0; k < 4; k++) {
        auto* m = makeMapping(k * kPage);
        ASSERT_TRUE(m != nullptr);
        ASSERT_TRUE(tree.apply(k * kPage, (k + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
    }
    quiesce(h);

    const uint64_t before = drawsOf(h, 0);
    ASSERT_TRUE(tree.apply(0, 4 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    ASSERT_EQ(uint64_t{4}, drawsOf(h, 0) - before);

    quiesce(h);
    ValidA::validate(tree, "four records, four slots");
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("four records, four slots");
}

// ─── Claim 2: a fully-covered child draws nothing ──────────────────────────

// §7.1: "Cover all sixteen and the parent's dispatch row is *fully-covered
// child*: the subtree detaches and every release rides a node deleter — zero
// records. The partial cover is the expensive shape, which is easy to invert."
//
// Easy to invert is the operative phrase: an implementation that drew a record
// on the detach row would be safe, would pass every reader-side test, and would
// silently consume records proportional to subtree size — blowing a budget
// derived on the assumption that it does not.
TEST(radix_deferred_release_a_fully_covered_child_draws_nothing) {
    Harness h(1, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));   // C1 root: 32 slots × 1 MiB
    const uint64_t slot = rdx::slotSpan(GA, 4);

    // Two disjoint sub-ranges inside slot 0 force a subdivision, so slot 0 ends
    // up holding a CHILD NODE rather than a leaf. (One range alone stays a leaf
    // with a sub-range field, which would make the unmap below an ordinary clear
    // and the test would pass for the wrong reason.)
    auto* a = makeMapping(0);
    auto* b = makeMapping(2 * kPage);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(tree.apply(2 * kPage, 3 * kPage - 1, b) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_TRUE(tree.nodeCount() > 1);   // the subdivision really happened

    const uint64_t before = drawsOf(h, 0);
    ASSERT_TRUE(tree.apply(0, slot - 1, nullptr) == rdx::ApplyStatus::Ok);
    ASSERT_EQ(uint64_t{0}, drawsOf(h, 0) - before);

    // The releases still happened — they rode the node deleters — so the records
    // are gone once the grace period ends.
    quiesce(h);
    ASSERT_EQ(size_t{1}, tree.nodeCount());
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("fully covered child");
}

// ─── Claim 3: the records recycle and every one comes home ─────────────────

TEST(radix_deferred_release_records_return_to_their_home_pool) {
    Harness h(1, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));

    // There is ONE population, per CPU, and it is also the per-attempt record
    // budget (D-059). Both readings are asserted, because a change that moved
    // the budget onto a second constant would leave the first assertion passing
    // and quietly reintroduce the possibility of an attempt wanting more records
    // than a barrier can bring home.
    const size_t population = poolOf(h, 0).population;
    ASSERT_EQ(static_cast<size_t>(rdx::kDefaultRecordsPerCpu), population);
    ASSERT_EQ(static_cast<unsigned>(population), h.releasePools.recordBudget());
    ASSERT_TRUE(poolOf(h, 0).atFullPopulation());

    // Churn well past the population, so a mechanism that consumed a record per
    // operation without returning it would run dry rather than merely look
    // untidy.
    for (unsigned round = 0; round < 4 * population; round++) {
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        ASSERT_TRUE(tree.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    }
    ASSERT_TRUE(tree.apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);

    // Every record home, and the population unchanged — §11's conservation
    // target, checked here as well as in the pool destructor because a pool that
    // leaks records only fails at teardown, long after the operation that did it.
    ASSERT_TRUE(poolOf(h, 0).atFullPopulation());
    ASSERT_EQ(population, poolOf(h, 0).population);
    ASSERT_TRUE(drawsOf(h, 0) >= 4 * population);
    ASSERT_EQ(uint64_t{0}, poolOf(h, 0).shortfalls.load(RELAXED));

    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("record recycling");
}

// The steady-state allocation claim, stated directly: "so a fixed population
// circulates and steady-state `munmap` churn allocates NOTHING". Checked with
// the oracle's per-type counter rather than by inspecting the pool, because the
// claim is about ALLOCATION and not about pool depth — an implementation that
// allocated a fresh record and pushed it onto the pool would keep the pool at
// depth and still be wrong.
TEST(radix_deferred_release_steady_state_churn_allocates_no_records) {
    Harness h(1, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));

    // The fixture's population is the accounting baseline, so this reads zero
    // for the pool's own records and counts only anything allocated since.
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::DeferredRelease>());

    for (unsigned round = 0; round < 200; round++) {
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        ASSERT_TRUE(tree.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
        ASSERT_EQ(size_t{0}, liveCountOf<rdx::DeferredRelease>());
    }

    ASSERT_TRUE(tree.apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("steady-state churn");
}

// ─── Claim 4: a shortfall is abandoned, replenished and retried ────────────

// A shortfall is only reachable when grace periods have stalled — the missing
// records are in flight through this CPU's own earlier retires, which is exactly
// what §7.1 says a short pool means. So CPU 1 holds a read section open, which
// stops the epoch advancing, and CPU 0 churns until its pool runs dry.
//
// **The handshake runs in BOTH directions**, and it has to. CPU 1 waits on CPU
// 0's *shortfall counter*, which is bumped at the draw — before the abandon,
// before the barrier — so by the time CPU 0 is blocked inside `barrier`, CPU 1
// has already been told to let go. That is the release direction.
//
// The acquire direction was missing and it cost a load-dependent failure: CPU 0
// must not start churning until CPU 1's read section is actually OPEN, because
// with the epoch free to advance every record comes home immediately and the pool
// never runs dry. The whole churn can then finish before the reader's thread has
// started, and the test fails on its own non-vacuity guard rather than on the
// mechanism — which is exactly the misleading shape the guard exists to prevent.
//
// It went unnoticed while the round count was derived from the per-CPU pool PLUS
// the reserve (262 rounds of slack); D-059 deleted the reserve and took the
// derivation to 32, which shrank the accidental margin 8x and surfaced it under
// TSan on a loaded machine.
//
// The deadlines are safety nets for the failure cases, not part of the mechanism:
// neither thread may spin forever, and each assertion then fails loudly rather
// than the test hanging with no diagnosis.
TEST(radix_deferred_release_a_shortfall_is_replenished_and_the_retry_succeeds) {
    constexpr size_t kCpus = 2;
    Harness h(kCpus, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));

    auto* seed = makeMapping(0);
    ASSERT_TRUE(seed != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, seed) == rdx::ApplyStatus::Ok);
    quiesce(h);

    std::atomic<bool> sawShortfall{false};
    std::atomic<bool> writerDone{false};
    // Set by CPU 1 once its read section is open, which is the precondition for
    // CPU 0's churn to be able to exhaust anything at all.
    std::atomic<bool> readerInSection{false};
    std::atomic<uint64_t> operations{0};
    std::atomic<size_t> failures{0};
    std::string firstError;
    std::mutex errorMutex;

    // The churn cap is derived rather than guessed: each displacing operation
    // draws exactly one record, so running well past the population with the
    // epoch stalled must exhaust it. One population again since D-059 — there is
    // no reserve to drain first, which is why this is the per-CPU figure alone.
    const size_t population = poolOf(h, 0).population;

    std::vector<std::thread> workers;

    // CPU 0 — the writer.
    workers.emplace_back([&] {
        kernel::test::bindThreadToCpu(0);
        try {
            // Wait for the reader's section. Without this the churn can complete
            // against a freely advancing epoch and exhaust nothing.
            const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!readerInSection.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= ready) {
                    throw AssertionFailure(std::string(
                        "the reader's section never opened, so the writer could not have "
                        "run short — this is the fixture failing, not the mechanism"));
                }
                std::this_thread::yield();
            }
            for (size_t round = 0; round < 8 * population; round++) {
                auto* m = makeMapping(0);
                if (!m) break;
                if (tree.apply(0, kPage - 1, m) != rdx::ApplyStatus::Ok) {
                    throw AssertionFailure(std::string("writer got a non-Ok status"));
                }
                operations.fetch_add(1, std::memory_order_relaxed);
                if (poolOf(h, 0).shortfalls.load(RELAXED) > 0) {
                    sawShortfall.store(true, std::memory_order_release);
                    break;
                }
            }
        } catch (const std::exception& e) {
            failures.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty()) firstError = e.what();
        }
        writerDone.store(true, std::memory_order_release);
    });

    // CPU 1 — the reader whose open section stalls the epoch.
    workers.emplace_back([&] {
        kernel::test::bindThreadToCpu(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        {
            kernel::rcu::ReadGuard guard(h.domain);
            // Announce only once the guard is constructed — the writer's
            // precondition is an OPEN section, not an intention to open one.
            readerInSection.store(true, std::memory_order_release);
            while (poolOf(h, 0).shortfalls.load(RELAXED) == 0 &&
                   !writerDone.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
        // Section closed. CPU 0's `barrier` — if it is in one — can now return,
        // its records come home, and its retried attempt succeeds.
    });

    for (auto& w : workers) w.join();
    kernel::test::bindThreadToCpu(0);

    if (failures.load() != 0) throw AssertionFailure("worker failure: " + firstError);

    // Non-vacuity: a run in which the pool never ran short has tested the
    // ordinary path twice and the shortfall path not at all.
    ASSERT_TRUE(sawShortfall.load());
    ASSERT_TRUE(poolOf(h, 0).shortfalls.load(RELAXED) > 0);
    ASSERT_TRUE(tree.stats().recordShortfalls.load(RELAXED) > 0);

    // And it recovered: the operation that hit the shortfall completed, so the
    // count of completed operations exceeds the count at which it ran dry.
    ASSERT_TRUE(operations.load() > 0);

    ASSERT_TRUE(tree.apply(0, kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ValidA::validate(tree, "shortfall recovery");
    ASSERT_TRUE(poolOf(h, 0).atFullPopulation());
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("shortfall recovery");
}

// ─── ITEM-084: what a partial cover actually draws ─────────────────────────
//
// DEC-068 sizes every (CPU x address space) pool at the edge sum — 230 records,
// ~14.4 KiB realised per CPU per address space — and ITEM-084 asks whether that
// eager sizing is right. The in-kernel stress cannot answer it: it unmaps
// exactly the span it placed, a FULL cover of one record, so its whole
// 1.5-million-unmap workload never draws more than two. The expensive shape has
// to be built on purpose, and these build it.
//
// They also test §7.1's dispatch claim, which nothing else does: the partial
// cover is the expensive row and the FULL cover is free, so an operation's cost
// is not monotonic in how much it unmaps. §7.1 states it exactly — "Cover all
// sixteen and the parent's dispatch row is fully-covered child: the subtree
// detaches and every release rides a node deleter — zero records. The partial
// cover is the expensive shape, which is easy to invert."

TEST(radix_partial_cover_draws_one_record_per_distinct_displaced_mapping) {
    Harness h;
    TreeA   tree;
    // Root at level 5, so the 16 page-sized slots below live in a level-6 CHILD
    // that has a parent to be detached from — which the fully-covered case needs
    // and a floor-level root could not provide.
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));

    constexpr unsigned kSlots = 16;
    std::vector<rdx::Mapping*> made;
    for (unsigned i = 0; i < kSlots; i++) {
        auto* m = makeMapping(i * kPage);
        ASSERT_TRUE(m != nullptr);
        made.push_back(m);
        ASSERT_TRUE(tree.apply(i * kPage, (i + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
    }

    // Fifteen of the sixteen — §7.1's own example. Each cleared slot names a
    // DISTINCT record, so each needs its own record: the dedup that makes a
    // fifteen-slot munmap of ONE mapping cost one record does nothing here.
    rdx::gDrawHistogram.reset();
    ASSERT_TRUE(tree.apply(0, (kSlots - 1) * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    ASSERT_EQ(uint64_t{kSlots - 1}, rdx::gDrawHistogram.max());

    quiesce(h);
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("partial cover draws");
}

TEST(radix_a_full_cover_draws_nothing_where_a_partial_one_draws_fifteen) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));

    constexpr unsigned kSlots = 16;
    for (unsigned i = 0; i < kSlots; i++) {
        auto* m = makeMapping(i * kPage);
        ASSERT_TRUE(m != nullptr);
        ASSERT_TRUE(tree.apply(i * kPage, (i + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
    }

    // The SAME node, unmapped one slot wider. Every release now rides a node
    // deleter through the detach, so the operation draws nothing at all — the
    // inversion §7.1 warns about, and the reason a sizing argument cannot be
    // read off "how big is the munmap".
    rdx::gDrawHistogram.reset();
    ASSERT_TRUE(tree.apply(0, kSlots * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    ASSERT_EQ(uint64_t{0}, rdx::gDrawHistogram.max());

    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("full cover draws nothing");
}

TEST(radix_draw_count_report) {
    // ITEM-084's harness-side evidence: the worst case is REACHABLE and scales
    // the way the edge sum says, which is what stops the observed max of 2 in
    // the kernel stress being read as "230 is over-provisioned by 100x".
    std::printf("\n  DeferredRelease draws for a partial cover of one node "
                "(ITEM-084; ceiling %u)\n", rdx::deferredReleaseBound(GA));
    std::printf("      distinct records in the node | cleared | drawn\n");

    for (unsigned filled : {2u, 4u, 8u, 16u}) {
        Harness h;
        TreeA   tree;
        ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));
        for (unsigned i = 0; i < filled; i++) {
            auto* m = makeMapping(i * kPage);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(tree.apply(i * kPage, (i + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
        }
        rdx::gDrawHistogram.reset();
        ASSERT_TRUE(tree.apply(0, (filled - 1) * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
        std::printf("      %28u | %7u | %5llu\n", filled, filled - 1,
                    (unsigned long long)rdx::gDrawHistogram.max());

        quiesce(h);
        ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        tree.destroyTree();
        quiesce(h);
        assertNoLiveObjects("draw count report");
    }
    std::printf("\n");
}

// ─── D-059: the record budget, and what it replaced ────────────────────────
//
// The per-CPU pool's population is ALSO the most records one attempt may hold.
// Exceeding it is `NeedsDecomposition`, not a shortfall: §6.5 splits the
// operation into units that fit, and termination stays local — every record an
// attempt can want is this CPU's own, so this CPU's own `barrier` brings it home.
//
// That identity is the whole design, so it is tested directly rather than
// inferred from the absence of hangs. A livelock in this path presents as no
// output at all, which is how ITEM-084's version of it survived a full suite:
// the previous answer was a shared per-address-space reserve, which made one
// CPU's progress depend on another CPU's records coming home and produced four
// separate liveness defects.
//
// The fixture deliberately starves the per-CPU pool, because at the shipping
// default of 32 the single-node dense shape fits in one attempt — which is
// exactly what the default is chosen for and exactly why it cannot exercise the
// mechanism for when it does not fit.

namespace {

struct StarvedPools {
    alignas(arch::CACHE_LINE_SIZE)
    rdx::DeferredReleasePool storage[arch::MAX_PROCESSOR_COUNT];
    rdx::DeferredReleasePools pools{};

    explicit StarvedPools(unsigned perCpu, size_t cpus = 1) {
        if (!pools.create(storage, cpus, perCpu)) {
            throw AssertionFailure(std::string("starved pool creation failed"));
        }
        // The record population is fixture infrastructure, exactly as the
        // Harness's own is — re-baseline so `assertNoLiveObjects` measures what
        // the TEST allocated rather than what the fixture did.
        setAccountingBaseline();
    }
    // Same guard, and for the same reason, as `~Harness` — see the note there.
    // `destroy()` asserts §11's conservation target, and an unguarded throw from
    // a destructor either terminates the process during an unwind (masking the
    // test's real failure) or, at best, reports the wrong one. Re-raised only
    // when nothing else is in flight.
    ~StarvedPools() noexcept(false) {
        try {
            pools.destroy();
        } catch (const std::exception& e) {
            if (std::uncaught_exceptions() > 0) {
                std::fprintf(stderr,
                             "[StarvedPools] teardown assert suppressed so the "
                             "failure already in flight is not masked: %s\n",
                             e.what());
                return;
            }
            throw;
        }
    }
};

// Fill one level-6 node's sixteen page slots with distinct records, then clear
// fifteen of them: §7.1's expensive shape, which WANTS fifteen records in one
// attempt. Whether it gets them is what the budget decides.
//
// The sixteen mappings are handed back so a caller can use them as its oracle:
// the whole point of a budget that decomposes is that the terminal state must be
// indistinguishable from the one attempt's, and "it did not hang" is not that.
void fillNode(TreeA& tree, std::vector<rdx::Mapping*>& made) {
    for (unsigned i = 0; i < 16; i++) {
        auto* m = makeMapping(i * kPage);
        ASSERT_TRUE(m != nullptr);
        made.push_back(m);
        ASSERT_TRUE(tree.apply(i * kPage, (i + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
    }
}

void fillNodeAndPartiallyClear(TreeA& tree, Harness& h) {
    std::vector<rdx::Mapping*> made;
    fillNode(tree, made);
    ASSERT_TRUE(tree.apply(0, 15 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
}

}  // namespace

// ─── The budget's own claim, stated as an inequality ───────────────────────
//
// "No attempt holds more records than the pool it draws from owns." Everything
// else about D-059 follows from it: it is what makes one `barrier` enough, and
// therefore what makes `consecutiveShortfalls <= 1` exact.
//
// Asserted against a shape that WANTS fifteen while the pool holds two, because
// the inequality is trivially true of a shape that fits — and a fixture where it
// fits is how the previous version of this test managed to be vacuous (R-7:
// `promo=3, perCpu=16`, so promotion covered the demand and the mechanism under
// test was never entered).
TEST(radix_no_attempt_exceeds_the_record_budget) {
    Harness h(1, 1);
    StarvedPools sp{2};                       // two records per CPU: far too few
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, sp.pools));
    ASSERT_EQ(unsigned{2}, sp.pools.recordBudget());

    std::vector<rdx::Mapping*> made;
    fillNode(tree, made);

    rdx::gDrawHistogram.reset();
    const uint64_t decompositionsBefore =
        tree.stats().decompositions.load(RELAXED);

    // Fifteen distinct records displaced, against a two-record budget.
    ASSERT_TRUE(tree.apply(0, 15 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    // **The inequality.** Without the budget gate this reads 15 — which is what
    // the same shape draws at the default budget, pinned by
    // `radix_partial_cover_draws_one_record_per_distinct_displaced_mapping`.
    ASSERT_TRUE(rdx::gDrawHistogram.max() <= sp.pools.recordBudget());

    // Non-vacuity, two ways. The overrun counter says the gate actually fired —
    // without it the operation would have completed as one attempt and this
    // test would be asserting the budget of a shape that never approached it.
    ASSERT_TRUE(tree.stats().recordBudgetOverruns.load(RELAXED) > 0);
    ASSERT_TRUE(tree.stats().decompositions.load(RELAXED) > decompositionsBefore);

    // **And the answer is still right.** A budget that decomposes is only
    // acceptable if the terminal state is the one attempt's: fifteen pages
    // cleared, the sixteenth untouched, and the survivor still naming its own
    // record with the count the single naming slot implies.
    quiesce(h);
    for (unsigned i = 0; i < 15; i++) {
        ASSERT_EQ(static_cast<rdx::Mapping*>(nullptr),
                  tree.lookup(i * kPage).mapping().address());
    }
    ASSERT_EQ(made[15], tree.lookup(15 * kPage).mapping().address());
    ASSERT_EQ(uint64_t{1}, made[15]->refcountRelaxed());
    ValidA::validate(tree, "record budget decomposition");

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    sp.pools.destroy();
    assertNoLiveObjects("record budget");
}

// The budget holds at the SHIPPING population too, and there it holds without
// decomposing — which is the other half of the claim and the reason 32 is the
// number. §7.1's own worked example must stay a single attempt.
TEST(radix_the_shipping_budget_absorbs_one_nodes_dense_shape) {
    Harness h(1, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));
    ASSERT_EQ(rdx::kDefaultRecordsPerCpu, h.releasePools.recordBudget());
    // 31 records for a 32-slot node, and the budget must clear it.
    ASSERT_TRUE(h.releasePools.recordBudget() >= rdx::singleNodeRecordBound(GA));

    std::vector<rdx::Mapping*> made;
    fillNode(tree, made);

    rdx::gDrawHistogram.reset();
    ASSERT_TRUE(tree.apply(0, 15 * kPage - 1, nullptr) == rdx::ApplyStatus::Ok);

    // One attempt, fifteen records, no overrun and no decomposition.
    ASSERT_EQ(uint64_t{15}, rdx::gDrawHistogram.max());
    ASSERT_EQ(uint64_t{0}, tree.stats().recordBudgetOverruns.load(RELAXED));

    quiesce(h);
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("shipping budget absorbs the dense shape");
}

TEST(radix_two_cpus_over_budget_at_once_both_finish) {
    // **The shape no single-CPU test can see**, kept from the version of this
    // test that covered the shared reserve — because the property being defended
    // has changed but the reason a single CPU cannot check it has not. D-057's
    // broken bound slipped past every single-CPU test in this file: a single CPU
    // never meets another at a shared resource.
    //
    // What is asserted now is that there IS no shared resource. Both CPUs run
    // over-budget shapes at once against a two-record-per-CPU population, and
    // each must finish using only its own pool and its own barrier —
    // `consecutiveShortfalls <= 1` in `runToCompletion` is a live assertion
    // during this run, so a CPU that needed a second round to recover throws
    // rather than passing quietly.
    constexpr size_t   kCpus   = 2;
    constexpr unsigned kRounds = 24;

    Harness h(kCpus, 1);
    StarvedPools sp{2, kCpus};
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, sp.pools));

    std::atomic<unsigned> completed{0};
    std::atomic<size_t>   failures{0};
    std::string           firstError;
    std::mutex            errorMutex;

    // Disjoint level-6 children under the same level-5 root: the two CPUs share
    // an address space — and therefore the pool ARRAY and the RCU domain —
    // without fighting over the same slots, so what this measures is the record
    // machinery and not ordinary claim contention.
    auto worker = [&](unsigned cpu, uint64_t basePage) {
        kernel::test::bindThreadToCpu(cpu);
        try {
            for (unsigned round = 0; round < kRounds; round++) {
                for (unsigned i = 0; i < 16; i++) {
                    const uint64_t va = (basePage + i) * kPage;
                    auto* m = makeMapping(va);
                    if (m == nullptr) throw AssertionFailure(std::string("record alloc failed"));
                    if (tree.apply(va, va + kPage - 1, m) != rdx::ApplyStatus::Ok) {
                        throw AssertionFailure(std::string("dense fill got a non-Ok status"));
                    }
                }
                // Fifteen of the sixteen: fifteen distinct records wanted,
                // against a two-record budget.
                const uint64_t lo = basePage * kPage;
                if (tree.apply(lo, lo + 15 * kPage - 1, nullptr) != rdx::ApplyStatus::Ok) {
                    throw AssertionFailure(std::string("wide clear got a non-Ok status"));
                }
                if (tree.apply(lo, lo + 16 * kPage - 1, nullptr) != rdx::ApplyStatus::Ok) {
                    throw AssertionFailure(std::string("cleanup clear got a non-Ok status"));
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            failures.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty()) firstError = e.what();
        }
    };

    std::vector<std::thread> workers;
    workers.emplace_back([&] { worker(0, 0); });
    workers.emplace_back([&] { worker(1, 16); });
    for (auto& w : workers) w.join();
    kernel::test::bindThreadToCpu(0);

    if (failures.load() != 0) throw AssertionFailure("worker failure: " + firstError);

    // **Liveness**: every round on both CPUs finished.
    ASSERT_EQ(kRounds * 2, completed.load());

    // Non-vacuity: both CPUs really went over budget, so both really exercised
    // decomposition while the other was doing the same.
    ASSERT_TRUE(tree.stats().recordBudgetOverruns.load(RELAXED) >= 2 * kRounds);

    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    sp.pools.destroy();
    assertNoLiveObjects("two CPUs over budget at once");
}

