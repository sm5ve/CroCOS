//
// radix-tree — the Mapping record (§5.4, §7.2; DEC-014 / 048 / 087 / 088 /
// 098 / 099).
//
// A leaf names an interposed record rather than inlining object, offset and
// flags in the slot word. That keeps slots ONE WORD, which the entire geometry
// analysis assumes — a 16 B slot does not survive it — and it gives per-mapping
// state a home for shootdown tracking, reverse-mapping membership and
// protection.
//
// ─── The offset derivation, and the two ways to get it wrong ───────────────
//
//     objectOffset + (faultVA - baseVA)
//
// BOTH terms matter, and both slips are silent wrong-frame bugs on the hot path
// — invisible to the partition invariant and to a shadow interval map, which
// model COVER rather than TRANSLATION.
//
//   - Without the record's own `objectOffset`, every mapping would begin at its
//     object's first byte: `mmap(fd, offset != 0)` is unrepresentable, and
//     DEC-080's per-bucket fragments (same VMObject, successive offsets) would
//     all alias the object's first frames.
//   - Anchoring the subtraction at the LEAF'S SLOT BASE instead of `baseVA` —
//     the base a descent actually has in hand, which makes it the tempting one —
//     resolves every address in a 64 KiB leaf to that leaf's first frame.
//
// Give translation its own test; nothing else will catch either.
//
// ─── Reference counting is N:1 (§7.2, DEC-048) ─────────────────────────────
//
// **One record is named by many leaf slots.** The count is the number of naming
// slots plus any outstanding lookup references, and it is maintained per
// NAMING-SLOT TRANSITION, never per operation. An enumeration of *events* was
// tried and is not exhaustive — see the table in §7.2; the shapes that break it
// are initial placement into k slots (no event row at all, so the record keeps
// whatever its constructor gave it, and the natural `1` is verbatim the
// premature free this rule exists to prevent) and the two subdivision shapes,
// which take opposite deltas through the same dispatch row.
//
// The record is constructed with a count of ZERO; the first publish takes it to
// one.
//

#ifndef CROCOS_RADIX_MAPPING_H
#define CROCOS_RADIX_MAPPING_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <core/atomic.h>
#include <mem/VMSubstrate.h>

#include <mem/radix/Ordering.h>

namespace kernel::mm {

    // The sibling spec's object. This tree's business ends at naming the
    // mapping, so the type stays opaque here — but note the three constraints
    // radix-tree.md exports to it, all of which ride the Dependencies table:
    // VMObject teardown must be deferred or incremental (DEC-071), its deferred
    // queue must have a LOAD-PATH drain site and not only an idle one
    // (DEC-104b), and rmap deregistration at destroy<Mapping> must be bounded
    // straight-line work (DEC-099). All three exist because destruction is
    // reachable from RCU deleters on the fault path.
    struct VMObject;

}

namespace kernel::mm::radix {

    // Protection bits. The set is the VMM's to extend; what is fixed here is
    // that protection lives on the MAPPING and not on the VMObject (DEC-087):
    // objects differ when the bytes can differ, mappings differ when the access
    // differs. Per-object protection would make a sub-range mprotect split the
    // OBJECT — reinventing the mapping layer inside the object layer and
    // breaking sharing, which is the ordinary case rather than the exception.
    enum class Protection : uint8_t {
        None    = 0,
        Read    = 1u << 0,
        Write   = 1u << 1,
        Execute = 1u << 2,
    };

    constexpr Protection operator|(Protection a, Protection b) {
        return static_cast<Protection>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    constexpr Protection operator&(Protection a, Protection b) {
        return static_cast<Protection>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }
    constexpr bool contains(Protection set, Protection bit) {
        return (static_cast<uint8_t>(set) & static_cast<uint8_t>(bit)) ==
               static_cast<uint8_t>(bit);
    }
    // DEC-088: mprotect clamps against the ceiling and nothing ever raises it.
    constexpr bool withinCeiling(Protection requested, Protection ceiling) {
        return (static_cast<uint8_t>(requested) & ~static_cast<uint8_t>(ceiling)) == 0;
    }

    struct Mapping {
        VMObject* object;

        // Byte offset within `object` that `baseVA` resolves to (DEC-087's
        // 2026-08-08 correction). Not derivable from anything else.
        uint64_t objectOffset;

        // The mapping's own base VA — the anchor for the offset derivation. NOT
        // the leaf's slot base; see the header comment.
        uint64_t baseVA;

        Protection protection;

        // DEC-088: fixed at mapping creation from whatever conveyed the right to
        // map, and never raised. The static half of W^X enforcement; DEC-070's
        // install-side interlock is the dynamic half. Added now rather than
        // later because the retrofit is asymmetric — one word today, versus
        // auditing every mprotect path and every mapping-creation site for a
        // ceiling that was implicitly RWX.
        Protection maxProtection;

        // Structural references (one per naming leaf slot) plus outstanding
        // lookup references. Constructed at ZERO — the first publish takes it
        // to one (§7.2).
        Atomic<uint64_t> refcount;

        // NOTE: a Mapping deliberately carries NO RetireHead and is never itself
        // the subject of a `retire` (§7.1, DEC-062). The relationship is N:1, so
        // two CPUs clearing two slots that name the same record would enqueue
        // the same intrusive linkage twice — a lost release (permanent leak) or
        // a duplicated one (the count reaches zero with a live naming slot, and
        // the next fault dereferences freed memory through the hot-path step
        // above). Nodes are immune only because a node is unlinked exactly once.
        // A single-threaded munmap of a three-slot region hits this
        // deterministically. Deferred releases ride their own DeferredRelease
        // record instead (§7.1, DEC-068), which arrives in Phase 3.

        Mapping(VMObject* obj, uint64_t offset, uint64_t base,
                Protection prot, Protection maxProt)
            : object(obj), objectOffset(offset), baseVA(base),
              protection(prot), maxProtection(maxProt) {
            assert(withinCeiling(prot, maxProt),
                   "radix Mapping: initial protection exceeds its max-protection ceiling");
            refcount.store(0, kPrivateInit);
        }

        Mapping(const Mapping&)            = delete;
        Mapping& operator=(const Mapping&) = delete;

        // §5.4's hot-path step. Spelled once, here, so no call site can
        // re-derive it with the wrong anchor.
        [[nodiscard]] uint64_t offsetFor(uint64_t faultVA) const {
            assert(faultVA >= baseVA,
                   "radix Mapping: fault VA below the mapping base — the offset derivation "
                   "would underflow");
            return objectOffset + (faultVA - baseVA);
        }

        // ─── Reference counting ────────────────────────────────────────────
        //
        // Orderings per §6.6/DEC-069. `fetch_add` is RELAXED: it is legal only
        // under an existing guarantee — the observing section (§7.3) or a
        // reference already held — so it needs to order nothing. `fetch_sub` is
        // RELEASE, and the ZERO-OBSERVING decrement takes an ACQUIRE fence
        // before destruction: every foreign CPU's accesses through its reference
        // must be visible to the destroying CPU before the memory is freed.
        //
        // This is the pairing a passing test cannot see. The design mostly hides
        // the hazard (a cache hit holds the count above zero throughout its
        // accesses), which is exactly the condition under which an implementer
        // writes relaxed RMWs and passes everything; the missing release/acquire
        // pair surfaces only as a foreign CPU's pre-release accesses racing the
        // destroying CPU's free.

        void acquireRef() {
            refcount.fetch_add(1, kRefcountAcquire);
        }

        // Returns true when this release observed the count reach zero, i.e.
        // when the caller now owns destruction. Destruction itself stays with
        // the counting mechanism and is NOT done here — a deleter may release a
        // reference but must not traverse the referent beyond its refcount word
        // (RCU-DEC-045).
        [[nodiscard]] bool releaseRef() { return releaseRefs(1); }

        // The aggregate form, for DEC-068's DeferredRelease records. One record
        // carries the whole deferred decrement for one distinct Mapping (§7.1),
        // so a munmap clearing fifteen slots that all name the same record
        // returns ONE record carrying `delta == 15` rather than fifteen records
        // carrying one each.
        //
        // A single fetch_sub rather than a loop of them, and that is not
        // micro-optimisation: a loop passes transiently through every
        // intermediate value, so a concurrent releaser could observe zero and
        // destroy the record while this one still has decrements to issue. The
        // count is only ever *observed* at zero by the actor that took it there.
        [[nodiscard]] bool releaseRefs(uint64_t n) {
            assert(n != 0, "radix Mapping: a zero-delta release is a drawn record that "
                           "never recorded anything — a leak of the record, and a sign the "
                           "draw and the row that justified it have come apart");
            const uint64_t prior = refcount.fetch_sub(n, kRefcountReleaseAcquire);
            assert(prior >= n,
                   "radix Mapping: release below zero — the double-release half of the N:1 "
                   "rule, whose other half is a leak");
            return prior == n;
        }

        [[nodiscard]] uint64_t refcountRelaxed() const { return refcount.load(kQuiescedRead); }
    };

    // ─── The record census (Phase 5) ───────────────────────────────────────
    //
    // The `Mapping` half of Node.h's node census, and the one the residue gate
    // cares about more: a leaked node costs 160 B, while a leaked record pins its
    // VMObject and every physical frame behind it. §11's "a Mapping outlives its
    // last naming slot and no longer" is a userspace-harness target with the
    // DEC-052 oracle behind it; in the kernel this pair of counters is all there
    // is.
    //
    // The allocation side is the STRESS's to count, not this header's: records
    // are minted by the VMM, and a `Mapping` constructor cannot tell a record
    // that will be published from one that will be discarded. Destruction is
    // singular here, which is the half that matters.
    struct MappingCensus {
        Atomic<uint64_t> created{0};
        Atomic<uint64_t> destroyed{0};

        [[nodiscard]] uint64_t liveQuiesced() const {
            return created.load(kCensusAccounting) - destroyed.load(kCensusAccounting);
        }
        void reset() {
            created.store(0, kCensusAccounting);
            destroyed.store(0, kCensusAccounting);
        }
    };

#ifdef CROCOS_RADIX_NODE_CENSUS
    inline MappingCensus gMappingCensus;
    inline void noteMappingCreated()   { gMappingCensus.created.fetch_add(1, kCensusAccounting); }
    inline void noteMappingDestroyed() { gMappingCensus.destroyed.fetch_add(1, kCensusAccounting); }
#else
    inline void noteMappingCreated() {}
    inline void noteMappingDestroyed() {}
#endif

    // ─── Releasing a reference (§7.1; RCU-DEC-006, the DEC-047 bug class) ──
    //
    // Takes a `SafePtr`, and **every** releaser takes one — deleters and
    // readers alike. An earlier version split the two, with a
    // `...FromDeleter` variant that made the freshness call and a plain one
    // that did not, on the reasoning that §7.1 names deleters only and that
    // putting the call on the reader's release would move an
    // `ensureTLBEntryFresh` onto the fault path, which is the cost DEC-082
    // rearranged a control block to avoid.
    //
    // **That split was wrong.** A release is an RMW on the record's count word,
    // and freshness is a property of one CPU's mapping. DEC-015 exists so the
    // fault path can close its section and block on a userspace pager, so the
    // thread that took the reference and the thread that drops it can be on
    // different CPUs — and the second one may hold a stale entry for the
    // record's page from a previous tenant. The reader's release is therefore
    // in exactly the position the deleter's is, and the only reason it looked
    // different is that §7.1's list was written about deleters.
    //
    // The counting mechanism owns destruction at zero — not the releaser
    // (§7.1 / RCU-DEC-045: a deleter may release a counted reference the retired
    // object holds, but must not traverse the referent beyond its refcount word;
    // destruction stays with the counting mechanism).
    //
    // Lives here rather than in CoreTree.h because DeferredRelease's deleter
    // needs it and CoreTree.h is downstream of that header.
    inline void releaseMappingRefs(VMSubstrate::SafePtr<Mapping> m, uint64_t n) {
        if (m->releaseRefs(n)) {
            noteMappingDestroyed();
            VMSubstrate::destroy(m);
        }
    }
    inline void releaseMappingRef(VMSubstrate::SafePtr<Mapping> m) {
        releaseMappingRefs(m, 1);
    }

    // The acquire side, for the same reason: `acquireRef` is an RMW on the same
    // word, and a reader reaching a record it has never touched before is the
    // first-touch case exactly.
    inline void acquireMappingRef(VMSubstrate::SafePtr<Mapping> m) { m->acquireRef(); }

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_MAPPING_H
