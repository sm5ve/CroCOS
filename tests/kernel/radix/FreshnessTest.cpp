//
// The freshness discipline, asserted rather than argued (D-049).
//
// Eight sites in this subsystem have failed to discharge `ensureTLBEntryFresh`
// on vmsmalloc-backed memory. Six were found by in-kernel stress boots, one per
// boot; two by a deliberate audit. **Zero were found by tests**, and not for
// want of coverage: 156 tests on two sanitizers pass identically whether every
// one of those calls is present or absent, because the userspace mock's
// `ensureTLBEntryFresh` cannot fail and — until now — recorded nothing. The bug
// class was literally unaskable here.
//
// These tests ask it. The mock records, per thread, which pages it was called
// for; each test below drives one path and asserts that the access it performs
// discharged freshness for the page it read. That is the proposition D-044
// settled on — freshness is a property of ONE CPU's mapping, so the question is
// always "did *this* thread pay for *this* page", never "did somebody".
//
// **What these cannot see**: whether a call was actually NEEDED. The mock has no
// page tables, so a superfluous call is invisible here and stays a question for
// `-icount` and D-042. These are one-directional — they catch the absent call,
// which is the direction that corrupts memory.
//
// Adding a site: drive it, clear the record immediately before the access under
// audit so the assertion names one access rather than "something touched this
// page", and mutation-test by deleting the discipline you are asserting. A
// freshness test that passes with the SafePtr removed is worse than no test.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/ClusterTable.h>
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
using TableA = rdx::ClusterTable<GA, CodecA>;

constexpr uint64_t kPage = 4096;

// RAII, because a thrown assertion between arm and disarm would leave recording
// on for every later test in the runner — cheap, but it would make one test's
// failure change another's behaviour, which is the worst kind of test coupling.
struct FreshnessRecorder {
    FreshnessRecorder()  { VS::test::armFreshnessRecording(); }
    ~FreshnessRecorder() { VS::test::disarmFreshnessRecording(); }
};

// Freshness is page-granular, so every claim about "this object's call" is really
// a claim about its page — which makes "are these two objects on the same page?"
// the precondition of any such assertion rather than a detail.
const void* pageOf(const void* p) {
    return reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(kPage - 1));
}

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

}  // namespace

// ─── The instrumentation itself ────────────────────────────────────────────
//
// First, because every test below is only as good as this: an assertion built on
// a recorder that silently records nothing would pass forever.

TEST(radix_freshness_recorder_sees_a_safeptr_access_and_ignores_address) {
    Harness h;
    FreshnessRecorder rec;

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    const VS::SafePtr<rdx::Mapping> p(m);

    VS::test::clearFreshnessRecord();
    ASSERT_EQ(uint64_t{0}, VS::test::freshnessCalls());
    ASSERT_FALSE(VS::test::pageWasMadeFresh(m));

    // `address()` reads no bytes and must therefore discharge NOTHING — the
    // distinction the whole API rests on. If this ever starts recording, every
    // assertion in this file becomes vacuous.
    (void)p.address();
    ASSERT_EQ(uint64_t{0}, VS::test::freshnessCalls());
    ASSERT_FALSE(VS::test::pageWasMadeFresh(m));

    // A real access does.
    (void)p->baseVA;
    ASSERT_TRUE(VS::test::freshnessCalls() > 0);
    ASSERT_TRUE(VS::test::pageWasMadeFresh(m));
    ASSERT_FALSE(VS::test::freshnessRecordOverflowed());

    VS::destroy(p);
}

TEST(radix_freshness_record_is_per_thread) {
    Harness h;
    FreshnessRecorder rec;

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    const VS::SafePtr<rdx::Mapping> p(m);
    (void)p->baseVA;
    ASSERT_TRUE(VS::test::pageWasMadeFresh(m));

    // The point of D-044: a call made on one CPU guarantees nothing to another.
    // The recorder models that, so an assertion here can never be satisfied by
    // some other thread's call — which is precisely the mistake the residual
    // page fault was.
    bool seenOnOtherThread = true;
    std::thread t([&] {
        seenOnOtherThread = VS::test::pageWasMadeFresh(m);
    });
    t.join();
    ASSERT_FALSE(seenOnOtherThread);

    VS::destroy(p);
}

// ─── The sites ─────────────────────────────────────────────────────────────

TEST(radix_lookup_result_discharges_freshness_on_the_mapping_body) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);

    FreshnessRecorder rec;
    // Scoped so the result's destructor drops its counted reference before the
    // teardown below — it is move-only and releases on destruction.
    {
        auto result = tree.lookup(0);
        ASSERT_TRUE(static_cast<bool>(result));

        // §1.1's last row, the one that was *wrong* rather than unidiomatic: the
        // result outlives the section and may be read on a CPU that never looked
        // it up, so the obligation travels with it and is paid HERE, at the read.
        VS::test::clearFreshnessRecord();
        (void)result.mapping()->offsetFor(0);
        ASSERT_TRUE(VS::test::pageWasMadeFresh(m));
    }
    // The suite's teardown idiom: clear the range so the record's last naming
    // slot goes away, quiesce so the deferred release actually lands, then the
    // tree. The record destroys itself at count zero — destroying it here would
    // be a double destroy, which the oracle catches.
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("freshness");
}

TEST(radix_enumerate_hands_out_a_pointer_that_discharges_freshness) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);

    // D-049 GAP 2. This emitted a raw `Mapping*` and every existing enumeration
    // test compared the emitted pointers by identity, so the defect was
    // invisible: a consumer reading a base VA or a protection out of an
    // enumerated record — /proc/maps, an mprotect walk, a fork copy loop — makes
    // a cross-CPU first touch through a pointer carrying no obligation.
    FreshnessRecorder rec;
    unsigned emitted = 0;
    bool     fresh   = false;
    tree.enumerate(0, kPage - 1, [&](VS::SafePtr<rdx::Mapping> found, uint64_t, uint64_t) {
        emitted++;
        VS::test::clearFreshnessRecord();
        (void)found->baseVA;                       // what a real consumer does
        fresh = VS::test::pageWasMadeFresh(found.address());
    });
    ASSERT_EQ(unsigned{1}, emitted);
    ASSERT_TRUE(fresh);

    // The suite's teardown idiom: clear the range so the record's last naming
    // slot goes away, quiesce so the deferred release actually lands, then the
    // tree. The record destroys itself at count zero — destroying it here would
    // be a double destroy, which the oracle catches.
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("freshness");
}

TEST(radix_apply_discharges_freshness_on_the_incoming_record) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);

    // D-049 GAP 1. The commit-phase `acquireRef` is an RMW on the incoming
    // record's count word. It used to run through a raw pointer, correct only
    // under an unwritten precondition — that the caller created the record on
    // the CPU now applying it — which no signature stated and nothing enforced.
    FreshnessRecorder rec;
    VS::test::clearFreshnessRecord();
    ASSERT_TRUE(tree.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(VS::test::pageWasMadeFresh(m));
    ASSERT_EQ(uint64_t{1}, m->refcountRelaxed());

    // The suite's teardown idiom: clear the range so the record's last naming
    // slot goes away, quiesce so the deferred release actually lands, then the
    // tree. The record destroys itself at count zero — destroying it here would
    // be a double destroy, which the oracle catches.
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("freshness");
}

TEST(radix_descent_pays_the_discipline_at_every_level) {
    // Stated as a COMPARISON rather than as a magic number, because the absolute
    // count is an implementation detail (how many slot and state-word reads one
    // level makes) while the property is not: a deeper descent stands on more
    // nodes, so it must discharge more. A walk that decoded a child pointer and
    // then read through it raw would flatten the difference — which is exactly
    // the §1.1 site "a node pointer decoded from a slot word".
    //
    // Counting DISTINCT PAGES instead would not work: two nodes of one size
    // class can share a slab page, so the page count understates the level count
    // by an amount that depends on the allocator's packing.
    // TWO distinct records at adjacent pages, not one mapping spanning both:
    // a single record is stored as ONE leaf carrying a sub-range, so a wide
    // root slot holds it without subdividing at all and the "deep" tree would
    // be exactly as shallow as the other one. Two records cannot share a slot,
    // which is what forces the subdivision down to the floor.
    auto callsForDescent = [](unsigned rootLevel, unsigned records) {
        Harness h;
        TreeA   tree;
        ASSERT_TRUE(tree.init(rootLevel, 0, h.domain, h.releasePools));
        for (unsigned k = 0; k < records; k++) {
            auto* m = makeMapping(k * kPage);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(tree.apply(k * kPage, (k + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
        }

        uint64_t calls = 0;
        {
            FreshnessRecorder rec;
            VS::test::clearFreshnessRecord();
            auto result = tree.lookup(0);
            ASSERT_TRUE(static_cast<bool>(result));
            calls = VS::test::freshnessCalls();
            ASSERT_FALSE(VS::test::freshnessRecordOverflowed());
        }
        ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        tree.destroyTree();
        quiesce(h);
        assertNoLiveObjects("freshness descent");
        return calls;
    };

    // A floor-level root: the leaf lives in the root itself, one node deep.
    const uint64_t shallow = callsForDescent(6, 1);
    // A C1 root over 1 MiB slots with two adjacent single-page records, so the
    // tree subdivides all the way to the floor (D-038 — one page does not make a
    // deep tree, and neither does one record). Several nodes stand between the
    // root and the leaf.
    const uint64_t deep = callsForDescent(4, 2);

    ASSERT_TRUE(shallow > 0);
    ASSERT_TRUE(deep > shallow);
}

// ─── The two DELETERS, and what R-6 actually turned out to be ──────────────
//
// R-6 reported that neither deleter's freshness discipline was asserted: delete the
// `SafePtr` at `CoreTree.h:112` or `DeferredRelease.h:193` and the suite stays
// green. Both mutations reproduce. **But they are green for a correct reason**, and
// finding that out is the substance of the finding.
//
// Those two `SafePtr`s are over the RETIRE SUBJECT ITSELF — the node, the record.
// RCU's `onPreTouch` already fired `ensureTLBEntryFresh` on that object before the
// deleter ran, and **freshness is PAGE-granular**, so the whole node body is
// already covered. The headers say `onPreTouch` covered "the `RetireHead` and
// NOTHING else", and at byte granularity that is true and at page granularity —
// the granularity that exists — it is not. So those calls are uniformity, not
// necessity, and no test can distinguish them from the hook that precedes them.
// (Their comments are corrected in place; this was also R-19's `CoreTree.h:107`
// row.)
//
// **The load-bearing call in each deleter is the one on the `Mapping`** — a
// separate allocation on a separate page that `onPreTouch` never touched, reached
// through a pointer read out of a slot word or a record field. That one is what
// these tests assert, and each begins by asserting its own premise: that the record
// really is on a different page from the retire subject, without which the
// assertion would be satisfied by `onPreTouch` and prove nothing.
//
// It is also worth knowing that the `Mapping` call is enforced STRUCTURALLY as
// well: `releaseMappingRefs` takes a `SafePtr<Mapping>` **by value**, so no caller
// can skip it without editing that signature. The tests below are what notice if
// someone does.
//
// Both tests clear the freshness record AFTER the retire, so what is measured is
// the deleter's own accesses and not the operation's.

TEST(radix_node_deleter_discharges_freshness_on_the_retired_node) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));
    FreshnessRecorder rec;

    // Two disjoint pages inside one slot force a subdivision, so slot 0 holds a
    // CHILD NODE. Clearing the whole slot then detaches and retires that child —
    // which is what puts `deleteRadixNode` on a real retire path rather than on
    // teardown's synchronous walk.
    const uint64_t slotSpan5 = rdx::slotSpan(GA, 5);

    auto* a = makeMapping(0);
    auto* b = makeMapping(2 * kPage);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(tree.apply(2 * kPage, 3 * kPage - 1, b) == rdx::ApplyStatus::Ok);
    // A SECOND naming slot for EACH record, in a different level-5 slot, so the
    // releases below take their counts 2 -> 1 and destroy neither. Both matter, and
    // for the same reason twice over: the counting mechanism's `destroy` discharges
    // freshness itself, so a record that reached zero would have its page made
    // fresh by `destroy` rather than by the deleter's RMW — and because `a` and `b`
    // are consecutive allocations in one size class they share a page, so
    // destroying EITHER would satisfy an assertion about the other. Both maskings
    // were measured, not supposed: earlier versions of this test passed with the
    // discipline removed, first via `destroy(a)` and then via `destroy(b)`.
    ASSERT_TRUE(tree.apply(slotSpan5, slotSpan5 + kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(tree.apply(slotSpan5 + 2 * kPage, slotSpan5 + 3 * kPage - 1, b)
                == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_TRUE(tree.nodeCount() > 1);            // the subdivision really happened
    ASSERT_EQ(uint64_t{2}, a->refcountRelaxed());
    ASSERT_EQ(uint64_t{2}, b->refcountRelaxed());

    // The child's address, captured while it is still linked. After the clear it
    // is retired and the pointer is the only way to name the page. Read before the
    // record is cleared, so this read's own freshness call cannot be mistaken for
    // the deleter's.
    const void* child = nullptr;
    {
        const uint64_t w = tree.root().slot(0).load(RELAXED);
        ASSERT_TRUE(CodecA::isChild(w));
        child = CodecA::decodeChild(w);
    }
    ASSERT_TRUE(child != nullptr);

    // The premise. If the node and the record it names shared a page, `onPreTouch`
    // on the node would satisfy everything below and the test would pass on a
    // deleter that never touched the record at all.
    ASSERT_TRUE(pageOf(child) != pageOf(a));

    ASSERT_TRUE(tree.apply(0, slotSpan5 - 1, nullptr) == rdx::ApplyStatus::Ok);

    // Retired, deleter not yet run. Clearing here is what makes the assertions
    // below about the DELETER: everything the operation itself touched is
    // forgotten at this point.
    VS::test::clearFreshnessRecord();
    ASSERT_FALSE(VS::test::pageWasMadeFresh(a));
    quiesce(h);                                   // the deleter runs here

    // **The claim.** A detached subtree's releases ride its node deleters (§7.1
    // draws no record for a fully-covered child), so this is `deleteRadixNode`
    // reading a leaf slot, decoding a record pointer and RMW-ing its count word —
    // a first touch of a page this CPU may hold a stale entry for.
    ASSERT_TRUE(VS::test::pageWasMadeFresh(a));
    // The node's own page is fresh too, but that says nothing: `onPreTouch` did it.
    ASSERT_TRUE(VS::test::pageWasMadeFresh(child));
    // Both survived, so no `destroy` ran anywhere on that page and the call above
    // can only have come from the deleter's own RMW.
    ASSERT_EQ(uint64_t{1}, a->refcountRelaxed());
    ASSERT_EQ(uint64_t{1}, b->refcountRelaxed());

    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("node deleter freshness");
}

TEST(radix_release_record_deleter_discharges_freshness_on_the_record) {
    Harness h;
    TreeA   tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));
    FreshnessRecorder rec;

    // ─── Getting the Mapping OFF the record pool's page ──────────────────
    //
    // The premise of the assertion below is that the record and the `Mapping` it
    // names sit on different pages. By default they do not: `DeferredRelease` is
    // 48 B and a `Mapping` falls in the same vmsmalloc size class, so the
    // fixture's whole 32-record population and the first few mappings a test
    // makes all land on ONE page — measured, not assumed; the first version of
    // this test tripped its own premise check.
    //
    // On that layout `onPreTouch` on the record already covers the `Mapping`, and
    // the assertion would hold on a deleter that never touched it. That is the
    // vacuity this file's header warns about: "a freshness test that passes with
    // the SafePtr removed is worse than no test."
    //
    // So the size class is filled past the pool's page before the record under
    // test is created. The filler is kept alive — freeing it would let the
    // allocator hand the vacated slots straight back — and destroyed at the end.
    std::vector<rdx::Mapping*> filler;
    for (unsigned i = 0; i < 128; i++) {
        auto* f = makeMapping(0);
        ASSERT_TRUE(f != nullptr);
        filler.push_back(f);
    }

    // TWO naming slots, so the deferred release takes `first`'s count 2 -> 1 rather
    // than to zero. At zero the counting mechanism's `destroy` runs, and `destroy`
    // discharges freshness itself — which would satisfy the assertion below on a
    // deleter whose count RMW skipped it. Measured: the first version of this test
    // passed with the discipline removed for exactly that reason.
    auto* first = makeMapping(0);
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(tree.apply(0, 2 * kPage - 1, first) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{2}, first->refcountRelaxed());

    // The pool is a LIFO Treiber stack, so the head is exactly the record the next
    // draw will take — which is how the test learns the address of a record it
    // never holds. One draw, so this is the record that gets retired.
    kernel::mm::radix::DeferredReleasePool& pool = h.releasePools.forCpu(0);
    const void* record = pool.head.load(RELAXED);
    ASSERT_TRUE(record != nullptr);

    // The premise, asserted rather than hoped for.
    ASSERT_TRUE(pageOf(record) != pageOf(first));

    // Overwrite, so `first` is displaced from a directly-written slot: one record
    // drawn, charged and retired.
    auto* second = makeMapping(0);
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, second) == rdx::ApplyStatus::Ok);

    VS::test::clearFreshnessRecord();
    ASSERT_FALSE(VS::test::pageWasMadeFresh(first));
    quiesce(h);                                   // deleteDeferredRelease runs here

    // **The claim.** §7.1: "the record was made fresh above; the RECORD IT NAMES is
    // a separate allocation on a separate page and owes its own call."
    ASSERT_TRUE(VS::test::pageWasMadeFresh(first));
    ASSERT_TRUE(VS::test::pageWasMadeFresh(record));   // again, `onPreTouch`'s doing
    // `first` survived, so no `destroy` ran and the call above is the deleter's.
    ASSERT_EQ(uint64_t{1}, first->refcountRelaxed());
    // ...and it came home, so the record the address names is the one that ran.
    ASSERT_TRUE(pool.atFullPopulation());

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    for (auto* f : filler) VS::destroy(VS::SafePtr<rdx::Mapping>(f));
    assertNoLiveObjects("record deleter freshness");
}

TEST(radix_pinned_storage_stays_exempt) {
    Harness h;
    FreshnessRecorder rec;

    // The other half of the discipline, and the half a "call everywhere" fix
    // would quietly destroy: pinned storage owes NOTHING. Its page-table entries
    // transition not-present -> present exactly once and never change (DEC-051b),
    // which is why DEC-082's round-4 amendment put the control block, the pool
    // heads and the RCU slot block there. A freshness call on the descent
    // cache's hot path is the cost that decision bought off.
    //
    // Driven through the release pools, which live in the fixture's pinned
    // storage: a draw and a return touch pool heads and record fields, and only
    // the RECORD's page — vmsmalloc memory — may be recorded.
    VS::test::clearFreshnessRecord();
    kernel::mm::radix::DeferredReleasePool& pool = h.releasePools.forCpu(0);
    kernel::mm::radix::DeferredRelease* r = pool.pop();
    ASSERT_TRUE(r != nullptr);
    ASSERT_FALSE(VS::test::pageWasMadeFresh(&pool));
    ASSERT_TRUE(VS::test::pageWasMadeFresh(r));    // the record IS vmsmalloc-backed
    pool.push(r);
}

TEST(radix_root_bucket_page_is_exempt) {
    // D-051's whole point, and the reason it is worth 4,096 B of window per
    // address space. Every descent opens by loading a bucket word; while the
    // root page was vmsmalloc memory that read owed a freshness call, and the
    // recycling it implied produced a real stale-read bug (D-039). The page now
    // comes from a pinned pool whose mapping never changes, so the call is gone
    // from the hottest read in the tree.
    //
    // Asserted as a NEGATIVE against a live positive in the same recording
    // window: the record's page must be made fresh (proving the recorder was
    // armed and the descent really ran) while the bucket page's must not. A
    // negative on its own would pass just as well with the instrumentation
    // switched off.
    Harness h;
    TableA  table;
    {
        kernel::rcu::DomainManagementLockGuard guard;
        ASSERT_TRUE(table.initLocked(h.domain, h.releasePools, h.rootPages,
                                     kernel::numa::DomainID{0}));
    }

    const uint64_t va  = 2 * rdx::slotSpan(GA, 0);
    const size_t   idx = rdx::bucketIndexFor<GA>(va);
    ASSERT_TRUE(table.createCluster(va, GA.defaultRootLevel) == rdx::ClusterStatus::Ok);

    TreeA t = table.treeFor(idx);
    auto* m = makeMapping(va);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);

    {
        FreshnessRecorder rec;
        VS::test::clearFreshnessRecord();
        auto result = t.lookup(va);
        ASSERT_TRUE(static_cast<bool>(result));
        (void)result.mapping()->baseVA;

        ASSERT_TRUE(VS::test::pageWasMadeFresh(m));                  // the live positive
        ASSERT_FALSE(VS::test::pageWasMadeFresh(table.buckets()));   // the exemption
    }

    ASSERT_TRUE(t.apply(va, va + rdx::nodeSpan(GA, GA.defaultRootLevel) - 1, nullptr)
                == rdx::ApplyStatus::Ok);
    quiesce(h);
    table.destroy();
    quiesce(h);
    assertNoLiveObjects("root page exempt");
}

// ─── D-042's missing multiplier ────────────────────────────────────────────

TEST(radix_freshness_calibration_report) {
    // D-042 prices per-level node freshness as "one `ensureTLBEntryFresh` per
    // level on the descent". That was true when the call sat where a node
    // pointer was DERIVED — seventeen sites, one per node per walk. D-044 then
    // moved it to per ACCESS (NodeRef holds a `SafePtr<void>`; every field goes
    // through `at<>`), which is correct and strictly more expensive, and nothing
    // has re-measured the multiplier since.
    //
    // This measures it. Not a cost in time — the harness has no page tables, so
    // what a call COSTS is a question for `-icount` and for cache-miss counters
    // on real silicon. What it can answer exactly is **how many calls a descent
    // makes, and against how many distinct pages**, which is the multiplier every
    // other estimate needs.
    std::printf("\n  freshness calls per LOOKUP, by tree depth "
                "(two adjacent single-page records, lookup of the first)\n");
    std::printf("      root level | depth | total | on nodes | on the record | "
                "distinct pages | per level\n");

    for (unsigned rootLevel = 6; rootLevel >= 1; rootLevel--) {
        Harness h;
        TreeA   tree;
        ASSERT_TRUE(tree.init(rootLevel, 0, h.domain, h.releasePools));

        // Two records, so the tree genuinely subdivides to the floor rather than
        // storing one sub-ranged leaf in the root's slot.
        rdx::Mapping* first = nullptr;
        for (unsigned k = 0; k < 2; k++) {
            auto* m = makeMapping(k * kPage);
            ASSERT_TRUE(m != nullptr);
            if (k == 0) first = m;
            ASSERT_TRUE(tree.apply(k * kPage, (k + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
        }

        uint64_t total = 0, onRecord = 0;
        size_t   pages = 0;
        {
            FreshnessRecorder rec;
            VS::test::clearFreshnessRecord();
            auto result = tree.lookup(0);
            ASSERT_TRUE(static_cast<bool>(result));
            total    = VS::test::freshnessCalls();
            onRecord = VS::test::freshnessCallsForPage(first);
            pages    = VS::test::freshnessPagesRecorded();
            ASSERT_FALSE(VS::test::freshnessRecordOverflowed());
        }
        const unsigned depth    = GA.levelCount - rootLevel + 1;
        const uint64_t onNodes  = total - onRecord;
        std::printf("      %10u | %5u | %5llu | %8llu | %13llu | %14zu | %9.2f\n",
                    rootLevel, depth, (unsigned long long)total,
                    (unsigned long long)onNodes, (unsigned long long)onRecord,
                    pages, double(onNodes) / double(depth));

        ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        tree.destroyTree();
        quiesce(h);
        assertNoLiveObjects("freshness calibration");
    }

    // The same walk on the write side, where a node's state word and its slots
    // are both touched — the descent cost D-042 quotes is the READ path's, and
    // an mmap pays more per level than a fault does.
    std::printf("\n  freshness calls per APPLY (overwrite of one page), by tree depth\n");
    std::printf("      root level | depth | total | distinct pages | per level\n");
    for (unsigned rootLevel = 6; rootLevel >= 1; rootLevel--) {
        Harness h;
        TreeA   tree;
        ASSERT_TRUE(tree.init(rootLevel, 0, h.domain, h.releasePools));
        for (unsigned k = 0; k < 2; k++) {
            auto* m = makeMapping(k * kPage);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(tree.apply(k * kPage, (k + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
        }
        auto* replacement = makeMapping(0);
        ASSERT_TRUE(replacement != nullptr);

        uint64_t total = 0;
        size_t   pages = 0;
        {
            FreshnessRecorder rec;
            VS::test::clearFreshnessRecord();
            ASSERT_TRUE(tree.apply(0, kPage - 1, replacement) == rdx::ApplyStatus::Ok);
            total = VS::test::freshnessCalls();
            pages = VS::test::freshnessPagesRecorded();
            ASSERT_FALSE(VS::test::freshnessRecordOverflowed());
        }
        const unsigned depth = GA.levelCount - rootLevel + 1;
        std::printf("      %10u | %5u | %5llu | %14zu | %9.2f\n",
                    rootLevel, depth, (unsigned long long)total, pages,
                    double(total) / double(depth));

        ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
        quiesce(h);
        tree.destroyTree();
        quiesce(h);
        assertNoLiveObjects("freshness calibration (write)");
    }
    std::printf("\n");
}
