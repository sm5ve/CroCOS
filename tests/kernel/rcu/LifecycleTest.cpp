//
// RCU-DEC-043 / RCU-DEC-044 — the runtime domain lifecycle and protectWord.
//
// These are the contracts radix-tree Phase 2 consumes and Phase 3 drives at
// address-space creation and teardown. Every clause below was added by a
// referee round rather than by the original design, and each has a silent
// failure mode:
//
//   - **init zeroes the block.** The zero-fill guarantee is per RESERVATION
//     (vmsmalloc DEC-051), so a freelist-recycled block carries the prior
//     tenant's state. Symptom: tryAdvance returns at the top forever — silent
//     no-reclamation, not a crash.
//   - **deinit poisons and THEN clears `initialized`.** Poison-last leaves the
//     nonzero-checked bool reading true and defeats the use-before-init assert
//     the poison exists to arm.
//   - **init/deinit take the lock themselves.** A caller holding
//     DomainManagementLockGuard across either self-deadlocks. Not driven here —
//     a deadlock test is a hang, not a failure — but the contract is asserted
//     by construction below: consumer reservation and framework init are two
//     separate acquisitions.
//   - **the runtime path fails cleanly.** It is reachable from address-space
//     creation, so untrusted userspace drives it and must get ENOMEM.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <core/atomic.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <rcu/RCU.h>           // real
#include "MockCpuLocal.h"
#include "MockInterruptContext.h"
#include "MockRcuEnv.h"
#include "DebugIntrospection.h"

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;

namespace {

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

struct Harness {
    explicit Harness(size_t cpus = 1, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
    }
    ~Harness() {
        VS::test::setStaticReservationFailAt(-1);
        // Before the arena goes away: the RCU block freelist is process-global
        // and would otherwise hand the next test a block pointing into
        // munmapped memory.
        rcu::test::resetDomainManagementState();
        VS::test::shutdown();
    }
};

struct Retirable {
    Core::rcu::RetireHead head;
    int value = 0;
};

int gDeleterRuns = 0;
void countingDeleter(Retirable* r) { gDeleterRuns++; (void)r; }

}  // namespace

// ─── init / deinit round-trip ──────────────────────────────────────────────

TEST(rcu_domain_init_deinit_round_trip) {
    Harness h;
    rcu::Domain d;
    ASSERT_FALSE(d.initialized());

    ASSERT_TRUE(d.init("lifecycle", 64));
    ASSERT_TRUE(d.initialized());
    ASSERT_EQ(arch::processorCount(), rcu::test::slotCount(d));
    ASSERT_EQ(size_t{64}, rcu::test::drainBatchBound(d));

    ASSERT_TRUE(d.deinit());
    ASSERT_FALSE(d.initialized());
}

TEST(rcu_domain_deinit_then_reinit_is_valid) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("first", 8));
    ASSERT_TRUE(d.deinit());

    // The second init draws a RECYCLED block. This is the case RCU-DEC-043 (i)
    // exists for: without the init-side zeroing, the block still carries the
    // first tenant's reader state and the domain silently stops reclaiming.
    ASSERT_TRUE(d.init("second", 16));
    ASSERT_TRUE(d.initialized());
    ASSERT_EQ(size_t{16}, rcu::test::drainBatchBound(d));

    // ...and it genuinely works: a retire on the recycled domain must reach its
    // deleter, which is the property "silent no-reclamation" would break.
    gDeleterRuns = 0;
    auto obj = VS::make<Retirable>();
    {
        rcu::ReadGuard g(d);
        rcu::retire<Retirable, &Retirable::head, &countingDeleter>(
            d, static_cast<Retirable*>(obj.raw()));
    }
    // The sanctioned teardown sequence: quiesce -> drainAllQuiescent -> deinit.
    // `barrier` is NOT a substitute — it leaves bags in non-Free states that
    // deinit's quiescence assert legitimately rejects.
    ASSERT_EQ(size_t{1}, rcu::drainAllQuiescent(d));
    ASSERT_EQ(1, gDeleterRuns);

    VS::destroy(obj);
    ASSERT_TRUE(d.deinit());
}

TEST(rcu_domain_blocks_recycle_rather_than_accumulate) {
    Harness h;
    // The high-water-mark claim vmsmalloc DEC-050 rests on: process churn must
    // reserve nothing after the first domain. Without recycling the
    // static-buffer window shrinks with CUMULATIVE process count until every
    // later fork fails — a failure that appears only after hours of uptime.
    //
    // Checked structurally: a long create/destroy loop must not exhaust the
    // mock's static-buffer region, which is sized for a few hundred pages.
    for (unsigned i = 0; i < 2000; i++) {
        rcu::Domain d;
        ASSERT_TRUE(d.init("churn", 8));
        ASSERT_TRUE(d.deinit());
    }
}

TEST(rcu_domain_recycled_blocks_are_reused_not_merely_freed) {
    Harness h;
    // Sharper than the churn test: the SAME storage must come back. Two
    // sequential domains take the same slot block, and a third concurrent one
    // takes a different block — which is what makes the freelist a freelist
    // rather than a leak with good manners.
    rcu::Domain a;
    ASSERT_TRUE(a.init("a", 8));
    const void* firstSlots = rcu::test::slotAddress(a);
    ASSERT_TRUE(a.deinit());

    rcu::Domain b;
    ASSERT_TRUE(b.init("b", 8));
    ASSERT_EQ(firstSlots, rcu::test::slotAddress(b));

    rcu::Domain c;
    ASSERT_TRUE(c.init("c", 8));
    ASSERT_TRUE(rcu::test::slotAddress(c) != firstSlots);

    ASSERT_TRUE(b.deinit());
    ASSERT_TRUE(c.deinit());
}

// ─── The failable path ─────────────────────────────────────────────────────

TEST(rcu_domain_init_fails_cleanly_when_storage_is_exhausted) {
    Harness h;
    // vmsmalloc DEC-051 (a) / RCU-DEC-043 (ii). Reachable from address-space
    // creation, so untrusted userspace drives it: it must return false, not
    // panic. The panicking reservation stays boot-only.
    VS::test::setStaticReservationFailAt(0);

    rcu::Domain d;
    ASSERT_FALSE(d.init("doomed", 8));
    ASSERT_FALSE(d.initialized());

    // And the failure leaves nothing behind: with reservations working again the
    // very next init succeeds.
    VS::test::setStaticReservationFailAt(-1);
    ASSERT_TRUE(d.init("recovered", 8));
    ASSERT_TRUE(d.deinit());
}

TEST(rcu_domain_a_failed_init_can_be_retried_on_the_same_object) {
    Harness h;
    // DEC-101's unwind sweeps the failure index across the creation sequence and
    // retries; a Domain left half-initialized by a failed init would make the
    // retry assert "init called twice".
    rcu::Domain d;
    VS::test::setStaticReservationFailAt(0);
    ASSERT_FALSE(d.init("attempt-1", 8));
    ASSERT_FALSE(d.init("attempt-2", 8));   // still failing, still clean
    VS::test::setStaticReservationFailAt(-1);
    ASSERT_TRUE(d.init("attempt-3", 8));
    ASSERT_TRUE(d.deinit());
}

// ─── deinit's ordering clause ──────────────────────────────────────────────

TEST(rcu_domain_deinit_leaves_initialized_false_not_poison) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("poison-order", 8));
    ASSERT_TRUE(d.deinit());

    // The whole point of the poison-then-clear ORDER. `initialized` is a plain
    // nonzero-checked bool inside the poisoned region: a fill that covered it
    // last would leave it reading 0xA5 — i.e. TRUE — and a dangling handle would
    // sail past the use-before-init assert and reclaim freely on a dead domain.
    //
    // In a debug build this is the poison actually running; in a release build
    // there is no poison and the clear is all there is. Both must end false.
    ASSERT_FALSE(d.initialized());
}

TEST(rcu_domain_deinit_asserts_quiescence) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("undrained", 8));

    gDeleterRuns = 0;
    auto obj = VS::make<Retirable>();
    {
        rcu::ReadGuard g(d);
        rcu::retire<Retirable, &Retirable::head, &countingDeleter>(
            d, static_cast<Retirable*>(obj.raw()));
    }

    // Retired but never drained: deinit must refuse rather than quietly drop the
    // object. The sanctioned sequence is quiesce -> drainAllQuiescent -> deinit,
    // and draining on the caller's behalf here would run deleters from a
    // teardown path that has already torn down what they touch.
    EXPECT_ASSERT_FAILURE(d.deinit());

    // Clean up properly, which is also the sanctioned sequence in miniature.
    (void)rcu::drainAllQuiescent(d);
    ASSERT_EQ(1, gDeleterRuns);
    ASSERT_TRUE(d.deinit());
    VS::destroy(obj);
}

// ─── drainAllQuiescent (RCU-DEC-035, exposed by RCU-DEC-043) ───────────────

TEST(rcu_drain_all_quiescent_empties_the_domain) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("teardown", 8));

    gDeleterRuns = 0;
    VS::SafePtr<Retirable> objs[4] = {
        VS::make<Retirable>(), VS::make<Retirable>(),
        VS::make<Retirable>(), VS::make<Retirable>(),
    };
    for (auto& o : objs) {
        rcu::ReadGuard g(d);
        rcu::retire<Retirable, &Retirable::head, &countingDeleter>(
            d, static_cast<Retirable*>(o.raw()));
    }

    // The distinguishing property against `barrier`: drainAllQuiescent force-
    // seals every OPEN bag, so the retirees still sitting in this CPU's open bag
    // — the ITEM-014 residue a barrier deliberately leaves — come home too.
    const size_t destroyed = rcu::drainAllQuiescent(d);
    ASSERT_EQ(size_t{4}, destroyed);
    ASSERT_EQ(4, gDeleterRuns);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));

    ASSERT_TRUE(d.deinit());
    for (auto& o : objs) VS::destroy(o);
}

// ─── protectWord (RCU-DEC-044) ─────────────────────────────────────────────

TEST(rcu_protect_word_returns_the_published_word) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("protect-word", 8));

    // The radix slot codec's shape: a tagged word that is NOT a valid pointer of
    // any type, which is exactly why `protect` cannot serve it — even the
    // uncompressed harness codec sets bit 0 on leaves.
    Atomic<uint64_t> slot{0};
    const uint64_t published = 0xDEADBEEF00001235ull;   // low bit set: a "leaf"
    slot.store(published, RELEASE);

    {
        rcu::ReadGuard g(d);
        ASSERT_EQ(published, rcu::protectWord(d, slot));
    }
    ASSERT_TRUE(d.deinit());
}

TEST(rcu_protect_word_asserts_a_section_is_open) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("protect-word-assert", 8));

    Atomic<uint64_t> slot{1};
    // The assert that catches §10's "a raw link load outside a section still
    // compiles, and in release silently" hazard. Without a veneer primitive the
    // consumer would supply this load itself and lose both the named ordering
    // constant and this check.
    EXPECT_ASSERT_FAILURE(rcu::protectWord(d, slot));

    ASSERT_TRUE(d.deinit());
}

// ─── DomainManagementLockGuard (RCU-DEC-043) ───────────────────────────────

TEST(rcu_domain_management_guard_is_reentrant_across_sequential_scopes) {
    Harness h;
    // The consumer shape: reserve under the guard, RELEASE it, then init — two
    // independent rare acquisitions. Holding the guard across init() is the
    // self-deadlock the contract forbids, and is deliberately not driven here
    // (a deadlock test is a hang, not a reportable failure).
    {
        rcu::DomainManagementLockGuard guard;
        // A consumer would reserve its own control-block storage here.
    }

    rcu::Domain d;
    ASSERT_TRUE(d.init("guarded", 8));
    ASSERT_TRUE(d.deinit());

    {
        rcu::DomainManagementLockGuard guard;   // and return it here
    }
}
