//
// radix-tree Phase 0 — the instrument's own gate.
//
// Phase 0's exit gate (spec §13) is: "A deliberately injected use-after-destroy
// and a deliberately injected intra-arena leak are both caught." Those are the
// first two tests here, and they are the reason this phase ships before Phase 1
// at all — an oracle nobody has tried to fool is indistinguishable from no
// oracle, and every lifetime rule in §7 is invisible without one.
//
// The use-after-destroy check has to run in a forked child: a caught ASan report
// terminates the process, so "did the oracle fire?" cannot be answered in-line.
// The child's success is its *death*.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <stddef.h>
#include <stdint.h>

#include "RadixHarness.h"
#include "RandomSource.h"

#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;

namespace {

// A stand-in for a tree node: the shape of a 32-slot node under ITEM-055's
// 64 B-aligned state word (288 B structural + alignment = 320), so it lands in
// the DEC-049 320 B class exactly as the real Node32 will. The real types
// arrive in Phase 1; what Phase 0 is testing is the instrument, not the tree.
struct alignas(64) ProbeObject {
    uint64_t marker;
    unsigned char filler[312];
};
static_assert(sizeof(ProbeObject) == 320);
static_assert(alignof(ProbeObject) == 64);

// Run `body` in a forked child and report whether the child failed to exit
// cleanly. Used to assert that a sanitizer report actually fires.
//
// The child's stderr goes to /dev/null by default: an expected ASan report
// printed into a passing test log trains the reader to skim sanitizer output,
// which is the one habit this whole harness exists to make unnecessary. Set
// CROCOS_RADIX_SHOW_CHILD=1 to see it.
template <typename F>
bool childDies(F&& body) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        throw AssertionFailure(std::string("childDies: fork failed"));
    }
    if (pid == 0) {
        // Set CROCOS_RADIX_SHOW_CHILD=1 to see the child's sanitizer output.
        // Needed to confirm the child is dying of the *injected* defect rather
        // than of something incidental — a death test that passes for the wrong
        // reason is worse than no death test, because it reports the oracle as
        // working.
        if (!std::getenv("CROCOS_RADIX_SHOW_CHILD")) {
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, 2); ::close(devnull); }
        }
        body();
        // Reached only if nothing trapped — which is the failure this reports.
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        throw AssertionFailure(std::string("childDies: waitpid failed"));
    }
    const bool exitedCleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return !exitedCleanly;
}

}  // namespace

// ─── Gate 1: an injected use-after-destroy is caught (DEC-052) ─────────────

TEST(radix_oracle_catches_use_after_destroy) {
    if (!kAsanPoisonActive) {
        // Not a silent pass: the TSan runner legitimately has no ASan, and
        // saying so is the difference between "this build cannot check it" and
        // "this build checked it and it was fine".
        std::fprintf(stderr, "[radix] SKIP use-after-destroy gate: built without ASan\n");
        return;
    }

    const bool caught = childDies([] {
        VS::test::initialize(1, 1);
        numa::test::configure(1, 1, &allToDomainZero);
        kernel::test::bindThreadToCpu(0);

        auto obj = VS::tryMake<ProbeObject>();
        obj->marker = 0x1234;
        void* raw = obj.raw();
        VS::destroy(obj);

        // The defect. In an uninstrumented arena this is a write into live,
        // addressable, unpoisoned memory and nothing notices — which is exactly
        // §10's "ASan cannot see intra-arena reuse" hazard, and exactly what
        // makes a use-after-grace-period here a silent wrong answer rather than
        // a crash.
        static_cast<ProbeObject*>(raw)->marker = 0x5678;

        // Defeat any chance the store above is optimised away.
        std::fprintf(stdout, "%llu",
                     static_cast<unsigned long long>(static_cast<ProbeObject*>(raw)->marker));
        _exit(0);
    });

    ASSERT_TRUE(caught);
}

// The oracle must ALSO survive slab recycling — the poison has to be re-applied
// per object, not once per arena. This is the case a "poison everything at
// shutdown" scheme would miss entirely, and it is the shape a use-after-grace-
// period actually takes: the node was reclaimed, its slot went back to the
// slab, and a stale reference is still pointing at it.
//
// Whether the *stale address itself* is handed back to the next allocation is
// vmsmalloc's business, not something this test may assume — so the parent
// first replays the allocation pattern in-process to find out which case it is
// in, and asserts accordingly. Asserting unconditionally would either be flaky
// or (the version this replaced) a vacuous ASSERT_TRUE(true).
TEST(radix_oracle_catches_use_after_slot_recycle) {
    if (!kAsanPoisonActive) {
        std::fprintf(stderr, "[radix] SKIP slot-recycle gate: built without ASan\n");
        return;
    }

    // Replay, without the defect, to learn whether the slot address is reused.
    bool sameSlotReturned;
    {
        Harness h;
        auto first = VS::tryMake<ProbeObject>();
        void* firstRaw = first.raw();
        VS::destroy(first);
        auto second = VS::tryMake<ProbeObject>();
        sameSlotReturned = (second.raw() == firstRaw);
        VS::destroy(second);
    }

    const bool caught = childDies([] {
        VS::test::initialize(1, 1);
        numa::test::configure(1, 1, &allToDomainZero);
        kernel::test::bindThreadToCpu(0);

        auto first = VS::tryMake<ProbeObject>();
        void* stale = first.raw();
        VS::destroy(first);

        // vmsmalloc hands the same slot back; `second` legitimately owns it now.
        auto second = VS::tryMake<ProbeObject>();
        (void)second;

        // A stale pointer to the *previous* tenant. If the recycled slot were
        // handed out without the poison being lifted and re-applied per object,
        // this write would land in live memory belonging to `second` and be
        // completely invisible.
        static_cast<ProbeObject*>(stale)->marker = 0xDEAD;
        std::fprintf(stdout, "%llu",
                     static_cast<unsigned long long>(static_cast<ProbeObject*>(stale)->marker));
        _exit(0);
    });

    if (sameSlotReturned) {
        // The allocator handed the identical slot back, so the "stale" pointer
        // now aliases a legitimately live object and the write is not a defect
        // at all. The oracle must NOT report it — a false positive here is what
        // gets the whole discipline deleted (§11's rationale for attaching the
        // poison to destroy rather than to the deleter).
        ASSERT_FALSE(caught);
    } else {
        // Different slot: the stale pointer addresses destroyed, poisoned
        // memory and the write must be caught.
        ASSERT_TRUE(caught);
    }
}

// ─── Gate 2: an injected intra-arena leak is caught ────────────────────────

TEST(radix_oracle_catches_intra_arena_leak) {
    Harness h;

    auto baseline = captureBaseline();

    // The defect: allocate and never destroy. LSan cannot see this — the arena
    // is one live mmap region and the object has no distinguishable footprint
    // (§10). The counters are the only detector.
    auto leaked = VS::tryMake<ProbeObject>();
    ASSERT_TRUE(static_cast<bool>(leaked));

    bool fired = false;
    try {
        assertBaseline(baseline, "deliberate leak");
    } catch (const AssertionFailure&) {
        fired = true;
    }
    ASSERT_TRUE(fired);

    // Clean up so the harness's own teardown is honest.
    VS::destroy(leaked);
    assertBaseline(baseline, "after cleanup");
}

// The opposite polarity: over-destruction. §11 requires both directions of the
// rule asserted, "Leak and double-release are opposite failures of one rule".
// A baseline check that only detects growth would pass a double-release.
TEST(radix_oracle_catches_over_destruction) {
    Harness h;

    auto a = VS::tryMake<ProbeObject>();
    auto b = VS::tryMake<ProbeObject>();
    ASSERT_EQ(2u, liveCountOf<ProbeObject>());

    auto baseline = captureBaseline();
    VS::destroy(a);

    bool fired = false;
    try {
        assertBaseline(baseline, "deliberate over-destruction");
    } catch (const AssertionFailure&) {
        fired = true;
    }
    ASSERT_TRUE(fired);

    VS::destroy(b);
    assertNoLiveObjects("cleanup");
}

// ─── Accounting behaves ────────────────────────────────────────────────────

TEST(radix_oracle_accounting_balances_over_churn) {
    Harness h;
    auto rng = streamFor(1);

    // Churn with a live set, so the arena genuinely recycles slots rather than
    // bump-allocating — recycling is what makes the poison discipline load-
    // bearing, and a churn test that never reuses a slot proves nothing.
    constexpr size_t kSlots = 32;
    kernel::mm::VMSubstrate::SafePtr<ProbeObject> live[kSlots] = {
        #define NULLP kernel::mm::VMSubstrate::SafePtr<ProbeObject>(nullptr),
        NULLP NULLP NULLP NULLP NULLP NULLP NULLP NULLP
        NULLP NULLP NULLP NULLP NULLP NULLP NULLP NULLP
        NULLP NULLP NULLP NULLP NULLP NULLP NULLP NULLP
        NULLP NULLP NULLP NULLP NULLP NULLP NULLP NULLP
        #undef NULLP
    };

    for (size_t i = 0; i < 4000; i++) {
        const size_t k = rng.below(kSlots);
        if (live[k]) {
            VS::destroy(live[k]);
            live[k] = kernel::mm::VMSubstrate::SafePtr<ProbeObject>(nullptr);
        } else {
            live[k] = VS::tryMake<ProbeObject>();
            ASSERT_TRUE(static_cast<bool>(live[k]));
            live[k]->marker = i;
        }
    }

    size_t stillLive = 0;
    for (size_t k = 0; k < kSlots; k++) if (live[k]) stillLive++;
    ASSERT_EQ(stillLive, liveCountOf<ProbeObject>());

    for (size_t k = 0; k < kSlots; k++) {
        if (live[k]) VS::destroy(live[k]);
    }
    assertNoLiveObjects("churn cycle");
}

TEST(radix_oracle_counts_are_per_type) {
    Harness h;
    struct alignas(64) OtherObject { unsigned char filler[160]; };

    auto a = VS::tryMake<ProbeObject>();
    auto b = VS::tryMake<OtherObject>();
    auto c = VS::tryMake<OtherObject>();

    ASSERT_EQ(1u, liveCountOf<ProbeObject>());
    ASSERT_EQ(2u, liveCountOf<OtherObject>());
    ASSERT_EQ(3u, totalLive());

    VS::destroy(a); VS::destroy(b); VS::destroy(c);
    assertNoLiveObjects("per-type counts");
}

// ─── Fault injection behaves ───────────────────────────────────────────────

TEST(radix_oracle_injects_allocation_failure) {
    Harness h;

    injectAllFailures();
    auto failed = VS::tryMake<ProbeObject>();
    ASSERT_FALSE(static_cast<bool>(failed));
    // A failed tryMake must account for nothing — an injected failure that
    // still incremented the live count would turn every unwind test into a
    // phantom leak report.
    assertNoLiveObjects("injected failure allocates nothing");

    resetInjection();
    auto ok = VS::tryMake<ProbeObject>();
    ASSERT_TRUE(static_cast<bool>(ok));
    VS::destroy(ok);
    assertNoLiveObjects("after reset");
}

TEST(radix_oracle_injects_failure_at_an_exact_index) {
    Harness h;

    // The creation-unwind primitive: succeed twice, fail the third, succeed
    // after. DEC-101's sequence is tested by sweeping this index.
    injectFailureAt(2);

    auto a = VS::tryMake<ProbeObject>();
    auto b = VS::tryMake<ProbeObject>();
    auto c = VS::tryMake<ProbeObject>();
    auto d = VS::tryMake<ProbeObject>();

    ASSERT_TRUE(static_cast<bool>(a));
    ASSERT_TRUE(static_cast<bool>(b));
    ASSERT_FALSE(static_cast<bool>(c));
    ASSERT_TRUE(static_cast<bool>(d));

    ASSERT_EQ(1u, injectionFailureCount());
    ASSERT_EQ(4u, injectionObservedCalls());

    VS::destroy(a); VS::destroy(b); VS::destroy(d);
    assertNoLiveObjects("exact-index injection");
}

TEST(radix_oracle_injects_sustained_exhaustion) {
    Harness h;
    injectFailuresAfter(1);

    auto a = VS::tryMake<ProbeObject>();
    ASSERT_TRUE(static_cast<bool>(a));
    for (int i = 0; i < 5; i++) {
        auto f = VS::tryMake<ProbeObject>();
        ASSERT_FALSE(static_cast<bool>(f));
    }
    ASSERT_EQ(5u, injectionFailureCount());

    VS::destroy(a);
    assertNoLiveObjects("sustained exhaustion");
}

// ─── The entropy source is reproducible ────────────────────────────────────

TEST(radix_random_source_is_reproducible_and_stream_independent) {
    RandomSource a(12345), b(12345);
    for (int i = 0; i < 64; i++) ASSERT_EQ(a.next(), b.next());

    // Distinct streams must not alias. A shared generator would make each CPU
    // thread's sequence depend on the interleaving, so a recorded seed would
    // reproduce nothing under concurrency — which is the whole point of having
    // one.
    auto s0 = streamFor(0);
    auto s1 = streamFor(1);
    bool differs = false;
    for (int i = 0; i < 16 && !differs; i++) differs = (s0.next() != s1.next());
    ASSERT_TRUE(differs);
}

TEST(radix_random_source_below_is_in_range) {
    auto rng = streamFor(7);
    for (int i = 0; i < 1000; i++) {
        const uint64_t bound = 1 + (i % 37);
        ASSERT_TRUE(rng.below(bound) < bound);
    }
    ASSERT_EQ(0u, rng.below(1));
    ASSERT_EQ(0u, rng.below(0));
}
