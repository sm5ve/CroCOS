//
// radix-tree — the memory-ordering table (§6.6; DEC-045 / DEC-069).
//
// "Every state-word primitive carries a named ordering, applied per-primitive
// rather than per-word."
//
// This file exists because of where the failure hides, not because named
// constants are tidy. **The claim->slot-read edge fails in a way this project's
// entire test matrix is blind to**: x86-64's `lock or` is a full barrier, so no
// QEMU config and no `-icount` run can expose a missing acquire; and ARMv8 TSan
// cannot either, because every access involved is a well-formed atomic and TSan
// does not model the ABSENCE of an acquire on one. That is the default release
// gate blind on both axes.
//
// And the failure is not a lost update but a DOUBLE one. With a relaxed claiming
// `fetch_or`, the state word and the slot are different addresses, so nothing
// orders the post-claim re-dispatch load against the RMW: the load may be
// serviced from a line the writer already held during its read pass. It then
// re-runs the dispatch on a PRE-CLAIM value, publishes over a mapping it
// believes absent, *and* increments the occupancy count a second time — leaving
// the node permanently non-reclaimable for the address space's life.
//
// So: review and these constants are the implementation; there is no runtime
// detector behind them. §11's countermeasure is a static check that every
// primitive is spelled from this table, which lives in
// `tests/kernel/radix/OrderingSpellingTest.cpp` — a load with no named constant
// is otherwise invisible to it precisely because it is not in the primitive
// list.
//

#ifndef CROCOS_RADIX_ORDERING_H
#define CROCOS_RADIX_ORDERING_H

#include <core/atomic.h>

namespace kernel::mm::radix {

    // ─── State word ────────────────────────────────────────────────────────

    // The post-claim re-dispatch load must not be serviced from a pre-claim
    // line. THE edge described above.
    inline constexpr MemoryOrder kClaimAcquire = ACQUIRE;

    // The writer's slot writes must be visible before the next claimer sees the
    // bit free.
    inline constexpr MemoryOrder kClaimRelease = RELEASE;

    // The count must not become visible ahead of the slot it describes. Pairs
    // with the NEXT CLAIMER's acquire fetch_or — not with any reader-side load;
    // the only reader-side count load is the advisory relaxed one below, which
    // deliberately synchronizes with nothing.
    inline constexpr MemoryOrder kOccupancyPublish = RELEASE;

    // The same edge in the other direction: the clearing store must be visible
    // before the count drops.
    inline constexpr MemoryOrder kOccupancyClear = RELEASE;

    // Prior writes visible before a descent-cache hit reads the mark.
    inline constexpr MemoryOrder kMarkStore = RELEASE;

    // Deliberately weak, and NAMING it advisory is the point — it stops someone
    // giving it acquire and then TRUSTING it. Candidacy is re-established from
    // the fetch_or prior at acquisition; the read pass holds no claim, so this
    // value may be stale by construction.
    inline constexpr MemoryOrder kAdvisoryOccupancyLoad = RELAXED;

    // ─── Slots ─────────────────────────────────────────────────────────────

    // What the old-or-complete-new guarantee actually rests on: the store being
    // single-word and release-ordered. Not a compare-exchange — every core-tree
    // slot writer holds that slot's claim bit, so the writer is exclusive and
    // there is no arbitration left to do at the word (DEC-055). Arbitration did
    // not vanish; it moved to the claim bit.
    inline constexpr MemoryOrder kSlotPublish = RELEASE;

    // RCU-DEC-022's kProtectedLinkLoad, reached through protectWord. The
    // decode-then-load shape is weaker than a plain pointer load, so address
    // dependencies cannot be relied on.
    inline constexpr MemoryOrder kSlotLoad = ACQUIRE;

    // A writer reading a slot it already holds the claim bit for. The claiming
    // fetch_or's acquire is what orders this, so the load itself needs nothing —
    // named separately from kSlotLoad so the two cannot be conflated: a reader
    // path using THIS constant would be dropping the protected-link-load
    // primitive entirely.
    //
    // **The precondition is a WHOLE-NODE claim, and it is not decorative** (R-9).
    // Legitimate uses are the three subtree walks — `detachmentIsFrozen`,
    // `markSubtree`, `retireSubtree` — which run over nodes `planDetachment`
    // claimed with the full valence mask, so every slot they read is frozen and
    // every child pointer they decode was already acquired by the read pass that
    // reached it.
    //
    // How each establishes that, precisely (this note got it wrong in the commit
    // that made the fix, which is the reviewed defect class — D-070):
    //
    //   * `detachmentIsFrozen` GATES on `holdsWholeNode` — it returns false rather
    //     than asserting, because a newcomer child is a legal retry, not a bug.
    //     It is the only one of the three that uses that predicate at all.
    //   * `markSubtree` and `retireSubtree` assert `claimsFullValence`, the
    //     mask-only form. BOTH need it, not just one: `retainClaim` clears `held`
    //     at the mark, so from `markSubtree`'s own body onward `holdsWholeNode` is
    //     already false while the claim BIT is still set.
    //
    // A slot the attempt merely DESCENDS THROUGH is not claimed — "the interlock
    // lives in the child, not in the parent slot" — so a walk that mixes written
    // and descended slots must use `kAttemptSlotLoad` below, not this.
    inline constexpr MemoryOrder kClaimedSlotLoad = RELAXED;

    // ─── The attempt's own walk over its sites (R-9) ───────────────────────
    //
    // `redispatchAgrees` and `commit` iterate every slot the operation's range
    // intersects, and the attempt holds the claim bit for the ones it will WRITE
    // and **not** for the ones it only descends through. So the set of slots
    // covered by the claiming `fetch_or`'s acquire is a strict subset of the set
    // this load reads, and `kClaimedSlotLoad`'s precondition is simply false here.
    //
    // That matters because the word is fed to `decodeChild` and the result is
    // dereferenced — the recursion descends through it. `kSlotLoad` above already
    // records why an address dependency does not rescue this: "the decode-then-load
    // shape is weaker than a plain pointer load, so address dependencies cannot be
    // relied on."
    //
    // **Unobservable on the hardware we run on, in both directions**, which is why
    // it sat wrong: x86-64 gives acquire semantics to every load, so no execution
    // can expose it, and the unit tests run on ARMv8 under TSan, which does not
    // model a missing acquire on a well-formed atomic. It is a defect for a port,
    // found by audit, and fixable at the price of an `ldar` on the mmap/munmap
    // path — not the fault path, which takes `kSlotLoad` already.
    //
    // Deliberately NOT spelled as `kSlotLoad`: that constant means "reached through
    // `protectWord`", i.e. RCU-DEC-022's protected link load, and these sites are
    // not that. Same ordering, different argument, so a different name.
    inline constexpr MemoryOrder kAttemptSlotLoad = ACQUIRE;

    // ─── Refcounts ─────────────────────────────────────────────────────────

    // Legal only under an existing guarantee — the observing section (§7.3) or a
    // reference already held — so it needs to order nothing.
    inline constexpr MemoryOrder kRefcountAcquire = RELAXED;

    // The last-owner discipline: every foreign CPU's accesses through its
    // reference must be visible to the destroying CPU before the memory is
    // freed. The zero-observing decrement additionally takes an ACQUIRE fence
    // before destroy<T>.
    inline constexpr MemoryOrder kRefcountRelease     = RELEASE;

    // §6.6/DEC-069 spells the structural decrement as RELEASE with an acquire
    // FENCE on the zero-observing one, and that is correct in the C++ model: the
    // destroying RMW reads a value in the release sequence headed by the last
    // holder's decrement, and an acquire fence sequenced after it completes the
    // edge. It is also **invisible to ThreadSanitizer**, which does not model
    // `atomic_thread_fence` — so on this project's default release gate the
    // recycled-record edge does not exist, and a reader's last read of a record
    // races the constructor of whatever lands in its recycled slab slot.
    //
    // Folding the acquire into the RMW says the same thing in a form the gate
    // can see. It is never weaker (an acquire RMW at zero is exactly what the
    // fence was there to provide), it costs one acquire on a decrement — no part
    // of the descent — and the alternative is a release gate that cannot see the
    // one edge whose absence is a use-after-free. See D-029.
    //
    // The former `kRefcountZeroFence = ACQUIRE` is deliberately GONE rather than
    // kept for documentation: a named ordering constant that nothing spells is
    // an invitation, and the §11 spelling check cannot tell an unused constant
    // from a live one.
    inline constexpr MemoryOrder kRefcountReleaseAcquire = ACQ_REL;

    // ─── Per-address-space control block (Phase 3) ─────────────────────────

    // The flag alone orders nothing — the GRACE PERIOD is the handshake. After
    // `synchronize` returns, every section that could have missed the flag has
    // exited, and every later attempt's entry fence orders its flag load after
    // the store.
    inline constexpr MemoryOrder kDyingFlagStore = RELEASE;
    inline constexpr MemoryOrder kDyingFlagLoad  = ACQUIRE;

    // The new root node's initialization must be visible before any observer can
    // reach it through the new word — the same edge kSlotPublish names. A failed
    // CAS re-reads and re-decodes.
    inline constexpr MemoryOrder kBucketPublishSuccess = RELEASE;
    inline constexpr MemoryOrder kBucketPublishFailure = ACQUIRE;

    // ─── Descent cache (Phase 4) ───────────────────────────────────────────

    // The COUNTERPART to kMarkStore. A release with no acquire opposite
    // establishes nothing, and §7.5 makes this read the memory-safety check that
    // a cached node's children are safe to follow.
    inline constexpr MemoryOrder kCacheFreshnessLoad = ACQUIRE;

    // DEC-079's calibration inputs: hit rate, atomic counts, per-entry thrash.
    // RELAXED and named so, for kPoolAccounting's reason — nothing reads them for
    // a correctness decision, and a name is what stops someone later giving one
    // an acquire and then TRUSTING it. They are read from every CPU and are
    // deliberately allowed to be a little stale relative to each other; a hit
    // rate computed from two counters sampled a nanosecond apart is still a hit
    // rate.
    //
    // **Single-writer, since D-076.** The counters live in the descent cache's
    // per-CPU `Row`, so each one has exactly one writer and any number of
    // readers, and the increment is a relaxed load/add/store rather than a
    // `fetch_add`. That is not a weakening of this constant, it is the reason it
    // can stay RELAXED under an even weaker primitive: an RMW is for arbitrating
    // between writers, and there are none to arbitrate. The atomicity that
    // remains is doing one job only — keeping the foreign-CPU READ out of
    // data-race territory, which matters because TSan is the release gate.
    //
    // Anything sharing this constant must satisfy the same premise. A counter
    // written by more than one CPU needs `fetch_add` back, and the name would
    // then be hiding the difference — so give it its own.
    inline constexpr MemoryOrder kCacheAccounting = RELAXED;

    // ─── The node census (Phase 5) ─────────────────────────────────────────
    //
    // The in-kernel stress's allocated/destroyed pair, which is what the §11
    // residue gate is measured against — the harness has the DEC-052 oracle and
    // the kernel has nothing, so this is the only thing that can answer "did
    // teardown come back to the bound".
    //
    // RELAXED, and the name is where the caveat lives: `live()` subtracts two
    // counters that were not sampled together, so it is only exact on a QUIESCED
    // tree with no CPU mid-operation — which is the only condition the residue
    // gate reads it under, and is stated at the read site as well as here.
    inline constexpr MemoryOrder kCensusAccounting = RELAXED;

    // ─── Non-shared accesses ───────────────────────────────────────────────
    //
    // Named rather than left bare so the §11 spelling check can be exhaustive:
    // an unnamed ordering is invisible to a check that looks for named ones, so
    // "this one genuinely needs nothing" has to be sayable IN the vocabulary
    // rather than by omission. Both are RELAXED, and the names are what carry
    // the argument for why that is sound.

    // Stores into an allocation nothing else can reach — a node under
    // construction, or a subdivision subtree being built before its publish.
    // Ordered by the publish that eventually makes it reachable (kSlotPublish),
    // which is a release and therefore covers everything written before it.
    inline constexpr MemoryOrder kPrivateInit = RELAXED;

    // Reads of a QUIESCED tree: validators, walks and node counts running
    // between operations with no concurrent mutator. §11 is explicit that
    // several of these must NOT be asserted mid-operation — occupancy is exact
    // only when no actor other than the reader holds a claim bit — so the name
    // also records where they may legally be used.
    inline constexpr MemoryOrder kQuiescedRead = RELAXED;

    // ─── DeferredRelease pools (Phase 3) ───────────────────────────────────

    // The deleter's cross-CPU push must publish its final accesses to the record
    // before the owner re-draws and rewrites it. Pop safety against ABA rests on
    // the pool being SINGLE-CONSUMER — only the owning CPU pops, true by
    // construction (per-CPU pool, pinned writers) and stated here so nobody adds
    // a second popper.
    inline constexpr MemoryOrder kPoolPush = RELEASE;
    inline constexpr MemoryOrder kPoolPop  = ACQUIRE;

    // The draw and shortfall counters. RELAXED and named so, because nothing reads
    // them for a correctness decision — they are evidence for sizing, not inputs
    // to a branch.
    inline constexpr MemoryOrder kPoolAccounting = RELAXED;

    // ─── Pool DEPTH is a correctness gate now (D-070) ──────────────────────
    //
    // It used to share `kPoolAccounting`, on the reasoning that "a stale depth
    // costs at most one unnecessary barrier or one extra shortfall retry, never a
    // wrong answer". **D-059 falsified the second half of that sentence.** With
    // the shared reserve gone, `if (!pool.atFullPopulation()) barrier(...)` is the
    // ONLY remedy in the shortfall path, and `consecutiveShortfalls <= 1` is an
    // exact assert — so a skipped barrier is not "one extra retry", it is the
    // assert firing in debug and a spin in release.
    //
    // The two directions are not symmetric, which is what the old comment missed:
    //
    //   * **under**-reporting depth costs a redundant barrier — harmless;
    //   * **over**-reporting skips the barrier — fatal.
    //
    // And over-reporting was reachable. `push` release-CASes `head` and THEN bumps
    // `depth`; a remote observer may see the bump without the CAS, so `depth` can
    // exceed what `pop` is able to find. The pair below closes it: the bump
    // publishes the CAS that preceded it, and the gate's load synchronizes with
    // the bump. ARM-only, and the unit tests run on ARM.
    //
    // `pop`'s decrement stays RELAXED deliberately — only the owner pops and only
    // the owner reads the gate, so its own decrements are already in program order,
    // and the direction a stale value could move is the harmless one.
    inline constexpr MemoryOrder kPoolDepthPublish = RELEASE;
    inline constexpr MemoryOrder kPoolDepthObserve = ACQUIRE;

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_ORDERING_H
