//
// radix-tree — the §6.3 write dispatch (DEC-040, extended by DEC-066/077).
//
// "A write into a claimed slot dispatches on the slot's current content, and
// the eight cases below are exhaustive."
//
// A PURE FUNCTION of (slot content, level, slot base, operation range, writes-a-
// value), which is why it can be tested to exhaustion before any concurrency
// exists. Everything the concurrent path adds around it — claiming the slot,
// re-dispatching after acquisition, publishing under a held bit — leaves this
// decision unchanged.
//
// ─── The eight rows ────────────────────────────────────────────────────────
//
//   empty, writing                     write the value            -> WriteLeaf
//   empty, clearing                    NO-OP: no store, no count  -> NoOp
//   leaf, fully covered, writing       overwrite in place         -> OverwriteLeaf
//   leaf/occupied, fully covered,
//     clearing                         store zero, then fetch_sub -> ClearSlot
//   leaf, partially covered, single
//     survivor, expressible here       rewrite the range in place -> ShrinkLeafInPlace
//   leaf, partially covered, two
//     survivors OR inexpressible       build a child SUBTREE      -> Subdivide
//   child node, partially covered      do NOT claim; descend      -> DescendIntoChild
//   child node, fully covered          detach the subtree         -> DetachChild
//
// ─── Three traps this encoding exists to avoid ─────────────────────────────
//
// **The empty-clear row is not decorative** (DEC-077). A decomposed munmap over
// a range with holes reaches it, and the wrong answer there is a spurious
// `fetch_sub` on a node whose count is already zero — whose borrow propagates
// into the spare bits and, had the mark been placed above the count instead of
// below it, would have cleared a dying mark. It is silent in release.
//
// **The subdivision row builds SUBTREES, not leaf pairs** (DEC-066). The
// minimal shapes (two leaves for a middle-punch munmap, three for a middle
// MAP_FIXED) are special cases. A survivor wider than one child slot becomes
// MANY leaves — a 4 KiB punch through a 32 MiB C0 leaf builds a C1->C2->C3
// spine with ~45 leaves naming the old record — and every one of them takes its
// own §7.2 `+1`. The row that shipped before DEC-066 was written for
// single-survivor shapes only, and a middle-punch munmap satisfied its LETTER
// while satisfying nobody's action: a literal implementer either left the
// punched page mapped (munmap returning success on a still-accessible address)
// or dropped a survivor (silently unmapping up to 52 KiB of live mapping).
//
// **Expressibility must be exact in both directions.** Rounding a survivor
// outward maps addresses the caller never asked for; rounding it inward unmaps
// live ones. So the shrink row applies only when the survivor's edges land
// exactly on this level's unit boundaries, and the fallback is subdivision —
// which is what "the range encoding is never load-bearing" means: insufficient
// resolution costs nodes, never semantics.
//

#ifndef CROCOS_RADIX_DISPATCH_H
#define CROCOS_RADIX_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/SlotCodec.h>

namespace kernel::mm::radix {

    enum class DispatchAction : uint8_t {
        // The slot is empty and the operation clears. No store, NO COUNT
        // DECREMENT. §6.3's eighth row.
        NoOp,

        // The slot is empty and the operation writes. The resulting leaf covers
        // exactly the operation's range within this slot — which is the whole
        // slot span for a fully-intersected slot, and a sub-range otherwise
        // (§6.5: writing the full span would map addresses the caller never
        // asked for).
        WriteLeaf,

        // The slot holds a leaf the operation fully covers, and the operation
        // writes. One store; the displaced record takes a deferred -1.
        OverwriteLeaf,

        // The slot holds a leaf the operation fully covers, and the operation
        // clears. Store zero, then a RELEASE fetch_sub on the owning node's
        // count.
        ClearSlot,

        // Edge-partial munmap's ordinary path: rewrite the slot word with the
        // shrunk range. No allocation, no subdivision, and — the part most
        // likely to be lost to a shared helper — NO REFERENCE-COUNT CHANGE. The
        // slot still names the same record.
        ShrinkLeafInPlace,

        // Build a child subtree over this slot's span. See buildPlan below.
        Subdivide,

        // The slot holds a child node the operation only partially covers. Do
        // NOT claim this slot — descend and claim inside the child. The
        // interlock lives in the child, not the parent slot, which is what
        // makes descent claim-free.
        DescendIntoChild,

        // The slot holds a child node the operation fully covers. Whole-node-
        // claim the subtree, mark every node in it, store the new value, retire
        // the subtree — subject to the detachment budget (§6.5), which is the
        // concurrent path's business, not this classification's.
        DetachChild,
    };

    // A half-open-free, inclusive absolute byte interval. Inclusive because the
    // codec's range encoding is (DEC-039) and mixing conventions across that
    // boundary is how off-by-one page bugs get in.
    struct Interval {
        uint64_t lo;
        uint64_t hi;   // inclusive
        constexpr bool empty() const { return hi < lo; }
        constexpr bool operator==(const Interval&) const = default;
    };

    struct DispatchResult {
        DispatchAction action;

        // Valid for WriteLeaf / OverwriteLeaf / ShrinkLeafInPlace: the sub-range
        // to encode into the slot word at this level.
        SubRange range{0, 0};

        // The surviving parts of an existing leaf that the operation does not
        // touch — what the subdivision must preserve. 0, 1 or 2 of them, and
        // the count alone selects between the shrink row and the subdivision
        // row when the survivor is expressible.
        unsigned survivorCount = 0;
        Interval survivors[2]{};

        // The operation's own range clipped to this slot — what a subdivision
        // must map (for a writing operation) or leave unmapped (for a clearing
        // one).
        Interval opRange{1, 0};
    };

    // `writesValue` distinguishes a placement/replacement from a clear. It is
    // the discriminant for four of the eight rows, and it is the operation's
    // property rather than the slot's — which is why it is a parameter here
    // rather than something inferred from the arguments.
    //
    // `opLo`/`opHi` are absolute and MUST already be clipped to this slot's
    // span; the caller has the slot base and the level, so clipping at the call
    // site keeps this function total.
    template <GeometryDescriptor G, typename Codec>
    DispatchResult dispatchSlot(uint64_t word, unsigned level, uint64_t slotBase,
                                uint64_t opLo, uint64_t opHi, bool writesValue) {
        const uint64_t span    = Geo<G>::slotSpan(level);
        const uint64_t slotEnd = slotBase + span - 1;

        assert(opLo <= opHi, "radix dispatch: empty operation range");
        assert(opLo >= slotBase && opHi <= slotEnd,
               "radix dispatch: operation range is not clipped to the slot");
        assert(slotBase % span == 0, "radix dispatch: slot base is not span-aligned");

        DispatchResult out;
        out.opRange = Interval{opLo, opHi};

        const bool coversWholeSlot = (opLo == slotBase && opHi == slotEnd);

        switch (Codec::kindOf(word)) {

        case SlotKind::Child:
            // Rows 4 and 5. Note the test is against the SLOT's span, not
            // against anything the child contains: a fully-covered child is
            // detached wholesale, and a partially-covered one is descended into
            // and never subdivided — subdividing here would detach a live
            // interior node.
            out.action = coversWholeSlot ? DispatchAction::DetachChild
                                         : DispatchAction::DescendIntoChild;
            return out;

        case SlotKind::Empty:
            if (!writesValue) {
                // Row 8. The one that must NOT decrement.
                out.action = DispatchAction::NoOp;
                return out;
            }
            // Row 1, as refined by §6.5's partially-intersected-empty-slot case.
            if (Codec::subRangeFor(slotBase, level, opLo, opHi, out.range)) {
                out.action = DispatchAction::WriteLeaf;
            } else {
                // The boundary is inexpressible at this level: §6.1's shortfall
                // spine. Costs one node per level of shortfall, which is why
                // that bound is level-dependent rather than flat.
                out.action = DispatchAction::Subdivide;
            }
            return out;

        case SlotKind::Leaf:
            break;
        }

        // ─── Leaf rows ─────────────────────────────────────────────────────

        uint64_t leafLo = 0, leafHi = 0;
        Codec::absoluteRange(word, slotBase, level, leafLo, leafHi);

        const bool overlaps = !(opHi < leafLo || opLo > leafHi);

        // Survivors: the leaf's covered range minus the operation's range.
        if (leafLo < opLo || !overlaps) {
            const uint64_t hi = overlaps ? (opLo - 1) : leafHi;
            if (hi >= leafLo) out.survivors[out.survivorCount++] = Interval{leafLo, hi};
        }
        if (overlaps && opHi < leafHi) {
            out.survivors[out.survivorCount++] = Interval{opHi + 1, leafHi};
        }

        if (!writesValue) {
            if (!overlaps) {
                // A clearing operation over a part of the slot the leaf does not
                // cover. Nothing to clear, and — the point — nothing to
                // decrement either. Reached by §6.5's decomposed munmap over a
                // range containing holes, where the hole is inside an occupied
                // slot rather than in an empty one.
                out.action = DispatchAction::NoOp;
                return out;
            }
            if (out.survivorCount == 0) {
                out.action = DispatchAction::ClearSlot;          // row 7
                return out;
            }
            if (out.survivorCount == 1 &&
                Codec::subRangeFor(slotBase, level,
                                   out.survivors[0].lo, out.survivors[0].hi, out.range)) {
                out.action = DispatchAction::ShrinkLeafInPlace;  // row 6
                return out;
            }
            // Two survivors (one slot cannot hold two ranges — invariant 3),
            // however expressible each is; or one survivor this level cannot
            // express.
            out.action = DispatchAction::Subdivide;              // row 3
            return out;
        }

        // Writing. With no survivors the slot ends up holding exactly one leaf
        // over the operation's range, so it is a single store — provided this
        // level can express that range.
        if (out.survivorCount == 0) {
            if (Codec::subRangeFor(slotBase, level, opLo, opHi, out.range)) {
                out.action = DispatchAction::OverwriteLeaf;      // row 2
            } else {
                out.action = DispatchAction::Subdivide;
            }
            return out;
        }

        // Survivors plus a new value: two distinct values wanting one slot.
        // Invariant 2 forbids it, so this subdivides regardless of how neatly
        // each part would encode on its own. This is the edge-mmap shape whose
        // §7.2 delta is 0 for the old record — the same dispatch row as the
        // middle punch, opposite delta, which is exactly why §7.2 is stated over
        // naming-slot transitions and not over events.
        out.action = DispatchAction::Subdivide;
        return out;
    }

    // ─── Naming-slot deltas (§7.2) ─────────────────────────────────────────
    //
    // Whether a row displaces the record the slot named, so the caller knows to
    // take the deferred -1. Deliberately a function of the ACTION rather than a
    // field of the result, so a new row cannot be added without landing here.
    //
    // The shrink row is the one to be careful with: it is the single slot
    // rewrite that must NOT touch the count, it is the one most likely to be
    // routed through the same helper as the three that do, and a spurious
    // decrement there is indistinguishable from the correct case until the
    // record is freed early.
    constexpr bool displacesNamedRecord(DispatchAction a) {
        switch (a) {
            case DispatchAction::OverwriteLeaf:
            case DispatchAction::ClearSlot:
                return true;
            case DispatchAction::ShrinkLeafInPlace:   // +-0 — the trap
            case DispatchAction::NoOp:
            case DispatchAction::WriteLeaf:
            case DispatchAction::Subdivide:           // handled per new leaf slot
            case DispatchAction::DescendIntoChild:
            case DispatchAction::DetachChild:         // rides the node deleters
                return false;
        }
        return false;
    }

    // Whether the row changes the owning node's occupancy count, and in which
    // direction. §6.3: incremented with a RELEASE fetch_add AFTER the slot
    // store, decremented with a RELEASE fetch_sub AFTER a clearing store.
    constexpr int occupancyDelta(DispatchAction a) {
        switch (a) {
            case DispatchAction::WriteLeaf:   return +1;   // empty -> occupied
            case DispatchAction::Subdivide:   return 0;    // caller decides: empty slot -> +1,
                                                           // occupied slot -> 0 (leaf becomes child)
            case DispatchAction::ClearSlot:   return -1;
            case DispatchAction::NoOp:
            case DispatchAction::OverwriteLeaf:
            case DispatchAction::ShrinkLeafInPlace:
            case DispatchAction::DescendIntoChild:
            case DispatchAction::DetachChild:
                return 0;
        }
        return 0;
    }

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_DISPATCH_H
