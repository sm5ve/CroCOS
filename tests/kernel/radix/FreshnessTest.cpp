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

#include <mem/radix/CoreTree.h>

#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;

constexpr uint64_t kPage = 4096;

// RAII, because a thrown assertion between arm and disarm would leave recording
// on for every later test in the runner — cheap, but it would make one test's
// failure change another's behaviour, which is the worst kind of test coupling.
struct FreshnessRecorder {
    FreshnessRecorder()  { VS::test::armFreshnessRecording(); }
    ~FreshnessRecorder() { VS::test::disarmFreshnessRecording(); }
};

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
