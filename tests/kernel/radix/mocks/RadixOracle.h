//
// radix-tree Phase 0 — the oracle's test-facing control surface.
//
// The mock <mem/VMSubstrate.h> declares only what make/tryMake/destroy call
// (registerType / noteConstructed / noteDestroyed / shouldInjectFailure). This
// header is what *tests* use: read the counters, script an allocation failure,
// assert a churn cycle returned to baseline.
//
// Split this way on purpose. The mock header is included by production sources
// compiled into the harness (the tree itself, vmsmalloc.cpp), and none of them
// should be able to see — let alone call — a function that injects an
// allocation failure or resets accounting.
//

#ifndef CROCOS_RADIX_ORACLE_H
#define CROCOS_RADIX_ORACLE_H

#include <stddef.h>
#include <stdint.h>

#include <mem/VMSubstrate.h>   // mock — for oracle::typeIdOf<T>

namespace CroCOSTest::radix {

    namespace VS = kernel::mm::VMSubstrate;

    // ─── Live-object accounting ────────────────────────────────────────────
    //
    // The only leak detector this harness can have: LSan sees the mock arena as
    // one live allocation, so an intra-arena leak has no footprint (§10). Every
    // make/tryMake increments and every destroy decrements, per type.

    size_t liveCount(size_t typeId);
    size_t totalLive();

    template <typename T>
    inline size_t liveCountOf() { return liveCount(VS::oracle::typeIdOf<T>()); }

    // Number of types registered so far, and their names/sizes — for the
    // failure message when a baseline assertion trips. A bare "3 objects
    // leaked" sends the reader back to the debugger; "3 × Node32" does not.
    size_t      registeredTypeCount();
    const char* typeName(size_t typeId);
    size_t      typeSize(size_t typeId);

    // A snapshot to compare against later. Taken by value so a test can hold
    // one across an operation that registers new types.
    struct Baseline {
        size_t counts[32];
        size_t types;
    };

    Baseline captureBaseline();

    // Assert live counts match the snapshot exactly, per type. Reports every
    // type that differs, with its name and both counts, and reports a *negative*
    // difference as loudly as a positive one: over-destruction is the
    // double-release half of the same rule, and radix-tree §11 requires both
    // directions asserted ("Leak and double-release are opposite failures of one
    // rule").
    void assertBaseline(const Baseline& b, const char* what);

    // Convenience for the common shape: "this churn returned to zero live
    // objects". Equivalent to assertBaseline over an all-zero snapshot.
    void assertNoLiveObjects(const char* what);

    // Drop all accounting. Type registrations survive (their ids are magic
    // statics inside the templates and cannot be un-assigned). Call between
    // tests, not inside one.
    void resetAccounting();

    // ─── Allocation fault injection ────────────────────────────────────────
    //
    // §9's allocation-failure rows and DEC-101's creation unwind need a
    // scriptable null from tryMake. Policies are deliberately deterministic and
    // countdown-shaped rather than probabilistic: an unwind test wants to fail
    // "the 4th allocation of this sequence" and then assert exactly what was
    // released, which a coin flip cannot express.

    // Fail nothing. The default, restored by resetInjection().
    void injectNothing();

    // Fail every tryMake from now on.
    void injectAllFailures();

    // Let `n` tryMake calls succeed, then fail exactly one, then resume
    // succeeding. This is the creation-unwind primitive: sweep n across the
    // sequence and each step's unwind gets exercised in turn.
    void injectFailureAt(size_t n);

    // Let `n` succeed, then fail everything after. Models sustained exhaustion
    // rather than a transient miss.
    void injectFailuresAfter(size_t n);

    void   resetInjection();
    // How many tryMake calls have been observed since the policy was set —
    // lets a test assert its sweep actually reached the site it was aiming at,
    // instead of silently running a no-op because the sequence was shorter than
    // it thought.
    size_t injectionObservedCalls();
    size_t injectionFailureCount();

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_ORACLE_H
