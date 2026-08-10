//
// radix-tree harness — the fixture every radix test opens with.
//
// One mmap'd mock arena, a NUMA topology, CPU binding, and a clean oracle. RAII
// so a thrown assertion still tears the arena down; a leaked mmap region across
// tests would make the *next* test's accounting meaningless, which is the kind
// of failure that gets misattributed for an afternoon.
//

#ifndef CROCOS_RADIX_HARNESS_H
#define CROCOS_RADIX_HARNESS_H

#include <stddef.h>
#include <arch.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock (radix, with the DEC-052 oracle)

#include <MockCpuLocal.h>      // kernel::test::bindThreadToCpu (vmsmalloc mocks)
#include <mem/radix/SlotCodec.h>
#include <mem/BlockPool.h>
#include <mem/radix/BucketCodec.h>
#include <mem/radix/DeferredRelease.h>
#include <rcu/RCU.h>
#include <MockInterruptContext.h>
#include <MockRcuEnv.h>
#include "../rcu/DebugIntrospection.h"

#include "mocks/RadixOracle.h"
#include <assert_support.h>
#include <cstdio>       // the teardown-failure report, which must not be swallowed
#include <exception>    // std::uncaught_exceptions — see ~Harness
#include <string>

namespace CroCOSTest::radix {

    namespace VS   = kernel::mm::VMSubstrate;
    namespace numa = kernel::numa;

    inline numa::DomainID allToDomainZero(arch::ProcessorID) { return numa::DomainID{0}; }

    // MockVMSubstrate.cpp's kRegionBytes. The codec is bound to the arena's real
    // extent rather than to the nominal 512 GiB window, so an address past the
    // arena is rejected instead of encoding cleanly and naming memory the tree
    // does not own.
    inline constexpr uint64_t kMockArenaBytes = uint64_t{64} * 1024 * 1024;

    struct Harness {
        // The tree's RCU domain. Per-address-space in production (DEC-072); one
        // per harness here, created and torn down with the arena.
        //
        // drainBatchBound is DEC-059's knob, provisionally 64: an UNBOUNDED
        // drain inherits arbitrarily many foreign deleters on a path a #PF
        // handler sits on, and this consumer retires from the fault path.
        kernel::rcu::Domain domain;

        // §7.1 / DEC-068's record population. Per ADDRESS SPACE in production —
        // the DEC-082 control block owns it — so one per harness here, alongside
        // the domain it shares a lifetime with.
        //
        // Sized at `kDefaultRecordsPerCpu`, which is what the kernel ships, and
        // that matters more since D-059 than it did before: the population is
        // also the per-attempt RECORD BUDGET, so a harness that over-provisioned
        // it would run every fixture with a budget no address space ever has and
        // would never see a decomposition the kernel will take. The tests that
        // want a small budget arm it deliberately (see `StarvedPools`).
        kernel::mm::radix::DeferredReleasePools releasePools{};

        // Storage for the pool array. In production this is the tail of the
        // control block's own pinned reservation (see ControlBlock); a fixture
        // that is not testing the address-space lifecycle just owns a buffer.
        // Sized at the cap because a test fixture can afford what a
        // per-address-space reservation cannot — which is the whole point of the
        // change that made this a pointer.
        alignas(arch::CACHE_LINE_SIZE)
        kernel::mm::radix::DeferredReleasePool poolStorage[arch::MAX_PROCESSOR_COUNT];

        // The root bucket page's pinned pool (D-051). Per ADDRESS-SPACE STORE in
        // production; a fixture that constructs a `ClusterTable` directly needs
        // one of its own. Per fixture rather than process-global, because the
        // arena it carves from is re-mmapped per fixture — the trap D-048 hit
        // when RCU's slot pool outlived one.
        kernel::mm::PinnedBlockPool rootPages;

        explicit Harness(size_t cpus = 1, size_t domains = 1) {
            VS::test::initialize(cpus, domains);
            numa::test::configure(cpus, domains, &allToDomainZero);
            arch::test::setProcessorCount(cpus);
            kernel::test::bindThreadToCpu(0);
            // Every slot word is encoded relative to the arena base, which mmap
            // chose a moment ago — so the codec must be re-bound per fixture,
            // not once per process.
            rootPages.init(sizeof(kernel::mm::radix::BucketTable));
            kernel::mm::radix::HarnessSlotBase::bind(
                static_cast<uintptr_t>(VS::arenaVirtualBase(0).value), kMockArenaBytes);
            // Accounting and injection are process-global (they must be — the
            // counters are read from every CPU thread), so each fixture resets
            // them rather than trusting the previous test to have been tidy.
            resetAccounting();
            resetInjection();

            if (!domain.init("radix", 64)) {
                VS::test::shutdown();
                throw AssertionFailure(std::string("radix Harness: domain init failed"));
            }
            // The shipping per-CPU population, so the harness runs against the
            // record budget the kernel runs against.
            if (!releasePools.create(
                    poolStorage, cpus, kernel::mm::radix::kDefaultRecordsPerCpu)) {
                (void)domain.deinit();
                VS::test::shutdown();
                throw AssertionFailure(
                    std::string("radix Harness: DeferredRelease pool creation failed"));
            }
            // The pool population is fixture infrastructure, not something a
            // test allocated — every count a test reads is relative to it.
            setAccountingBaseline();
        }

        // ─── Why this destructor is noexcept(false) and catches ───────────
        //
        // `releasePools.destroy()` asserts §11's record-conservation target, and
        // an assert in this harness THROWS. Left unguarded in a destructor that
        // is implicitly `noexcept`, that throw is fatal twice over:
        //
        //   * If a test-body assertion is already propagating, a second throw
        //     during unwinding is `std::terminate`. The original failure is lost
        //     and every diagnostic blames conservation instead — so a test that
        //     failed for one reason reports a different, misleading one. That
        //     masking is real and it cost a debugging session: an
        //     `ASSERT_EQ(0, promotionCount())` failure surfaced as "the pools did
        //     not hold their creation population at teardown".
        //   * Either way an escaping throw skips everything below it, so the
        //     domain is never deinitialised, the process-global RCU freelist is
        //     never reset and the arena is never shut down — and the NEXT test
        //     starts against a live domain and a block pointing into munmapped
        //     memory. One failing test would cascade into the rest of the file.
        //
        // So the conservation failure is captured rather than thrown, the rest
        // of teardown ALWAYS runs, and the failure is re-raised only when it is
        // the sole one in flight — which requires opting out of the implicit
        // `noexcept`.
        ~Harness() noexcept(false) {
            bool        teardownFailed = false;
            std::string teardownError;

            // Teardown order matters and is the consumer's obligation:
            // drainAllQuiescent (its no-new-users precondition discharged here by
            // the tests being single-threaded at this point, and by joined
            // threads in the concurrent ones), THEN deinit, THEN the arena.
            if (domain.initialized()) {
                // §7.4's order: drainAllQuiescent FIRST, so every in-flight
                // record's deleter has run and pushed it home, THEN free the
                // pools, THEN deinit. Freeing the pools first would destroy
                // records still linked into an undrained bag.
                (void)kernel::rcu::drainAllQuiescent(domain);
                try {
                    releasePools.destroy();
                } catch (const std::exception& e) {
                    teardownFailed = true;
                    teardownError  = e.what();
                }
                (void)domain.deinit();
            }
            // The RCU block freelist is process-global while the arena is
            // per-test; without this the next test gets a block pointing into
            // munmapped memory.
            kernel::rcu::test::resetDomainManagementState();
            resetInjection();
            VS::test::shutdown();

            if (!teardownFailed) return;
            if (std::uncaught_exceptions() > 0) {
                // A failure is already on its way to the runner. Report this one
                // where it cannot displace that one.
                std::fprintf(stderr,
                             "[radix Harness] teardown assert suppressed so the "
                             "failure already in flight is not masked: %s\n",
                             teardownError.c_str());
                return;
            }
            throw AssertionFailure(teardownError);
        }

        Harness(const Harness&) = delete;
        Harness& operator=(const Harness&) = delete;
    };

    // **Reclamation is DEFERRED.** A retired node's Mapping references come home
    // only when its deleter runs at grace-period end, so between an operation
    // and its grace period the structural counts legitimately exceed the naming-
    // slot census and the live-object counters legitimately exceed the reachable
    // set. Neither is a leak.
    //
    // Any assertion about structural counts, live-object accounting or node
    // counts must therefore quiesce first — which is not a testing artifact but
    // §11's own instruction: "Tree memory returns to baseline after churn — in-
    // kernel stress with node accounting, AFTER AN EXPLICIT tryAdvance PUMP
    // (DEC-060), since RCU pulls progress from the retire path and an idle CPU
    // strands its last bag."
    //
    // drainAllQuiescent rather than tryAdvance because a single advance does not
    // complete a grace period, and because the caller here genuinely satisfies
    // its no-new-users precondition: single-threaded, or with every worker
    // joined.
    inline void quiesce(Harness& h) {
        (void)kernel::rcu::drainAllQuiescent(h.domain);
    }

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_HARNESS_H
