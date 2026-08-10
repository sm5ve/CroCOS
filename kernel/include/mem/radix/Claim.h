//
// radix-tree — claiming (§6.2; DEC-036 / 038 / 046) and the claim set (§6.1).
//
// A writer reserves the slots it will write by setting bits in the owning node's
// claim bitmap. **Descending THROUGH a slot takes no claim; only writing one
// does** — a child-pointer slot cannot legitimately change except by reclamation
// or detachment, and both acquire the CHILD's state word, which a writer working
// inside that child already holds. The interlock lives in the child, not in the
// parent slot.
//
// ─── Why claiming is speculate-then-validate ───────────────────────────────
//
// `fetch_or` cannot be made conditional. So: OR the mask, examine the returned
// prior, and touch NO SLOT unless the prior shows the mark clear and every bit
// of the mask clear. Otherwise clear `mask & ~prior` — **not `mask`**, which
// would clear bits belonging to whoever won.
//
// ─── One fetch_or per node per acquisition (DEC-046) ───────────────────────
//
// The validation is NOT repeated, and repeating it is not merely redundant but
// *unsafe*. The only validation primitive is fetch_or-and-inspect-prior, which
// is not idempotent: a second issue returns the writer's OWN bits in `prior`,
// the predicate is non-zero, the writer concludes it lost, releases
// `mask & ~prior` == 0 (clearing nothing) and retries — **deterministically, on
// an uncontended tree, with one CPU**. It presents as a hang with no assert, no
// crash and no sanitizer report, under which the first hypothesis is contention
// rather than self-abort.
//
// The reason one validation suffices: from the instant a writer validly holds a
// bit, **no actor can mark that node**. All three mark paths — reclamation,
// detachment and teardown — go through a whole-node `fetch_or`, which fails
// against any held bit.
//
// This file enforces the once-per-node rule STRUCTURALLY rather than by a
// counter: the claim set merges masks by node, so a node cannot appear twice,
// and acquisition asserts each entry is issued exactly once.
//

#ifndef CROCOS_RADIX_CLAIM_H
#define CROCOS_RADIX_CLAIM_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <core/atomic.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/Node.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    // DEC-077, provisionally 64, a named constant beside drainBatchBound. The
    // budget is CUMULATIVE PER ATTEMPT — summed over every fully-covered subtree
    // the operation would detach, not per subtree alone. A MAP_FIXED covering
    // several sibling subtrees, each individually small, sums them; the per-
    // subtree reading silently undersizes the fixed-capacity claim set by up to
    // a factor of the valence.
    //
    // Bracketing shapes under the kernel geometry: a full C1 subtree is
    // 1 + 32 = 33 nodes (under budget, never decomposes) and a full C0 subtree is
    // 1057 (decomposes into 32 C1 units of 33 plus a final unit). So real
    // munmap/MAP_FIXED shapes below the multi-MiB range almost never decompose,
    // while a unit's mark pass and claim set stay cache-resident.
    //
    // **The bracket above is what retains 64, and it has to be** (R-21). Phase 5
    // was supposed to measure it and the measurement is vacuous: the in-kernel
    // stress caps a wide unmap at 16 granules, which detaches as 1 + 16 = 17, so
    // its reported `max=17` is the fixture's arithmetic and its
    // `decompositions=0` is structural. The budget is therefore held by analysis,
    // not by evidence — which is sound, but it is not what "Phase 5 measures it"
    // promised, and widening that fixture is an open follow-up.
    inline constexpr unsigned kDetachBudget = 64;

    // DEC-095's `scanChunk`: the most slots a chunked scan or enumeration
    // examines inside ONE read section. Provisional at 64, deliberately the same
    // figure as the detachment budget — both bound the same quantity, which is
    // how much non-preemptible, domain-wide-EBR-stalling work one section may
    // hold — and a calibration output rather than a design commitment.
    inline constexpr unsigned kScanChunk = 64;

    // §6.2's acquire predicate and its prior.
    struct ClaimOutcome {
        bool     acquired;
        // The state word as it was BEFORE the OR. Valid only when acquired, and
        // it is the whole point of the primitive: §6.4's reclamation decision is
        // computed from the priors the claims froze, because a whole-node claim
        // makes the occupancy count it carries EXACT — no other actor can
        // publish into the node.
        uint64_t prior;
    };

    // Speculate-then-validate. `mask` is the full set of bits wanted at this
    // node, merged (a write-site mask and a candidacy whole-node mask are ONE
    // fetch_or, never two — two is DEC-046's self-livelock).
    [[nodiscard]] inline ClaimOutcome tryClaim(Atomic<uint64_t>& stateWord, uint64_t mask) {
        assert(mask != 0, "radix: an empty claim mask means the site should not be in the set");
        const uint64_t prior = stateWord.fetch_or(mask, kClaimAcquire);

        if ((prior & (mask | state::kMarkMask)) == 0) {
            return ClaimOutcome{true, prior};
        }

        // Clear only the bits WE set. `~mask` would clear bits belonging to
        // whoever won, and the resulting corruption is a node whose claim
        // bitmap says free while a live writer is mid-commit inside it.
        const uint64_t ours = mask & ~prior;
        if (ours != 0) stateWord.fetch_and(~ours, kClaimRelease);
        return ClaimOutcome{false, prior};
    }

    inline void releaseClaim(Atomic<uint64_t>& stateWord, uint64_t mask) {
        if (mask == 0) return;
        stateWord.fetch_and(~mask, kClaimRelease);
    }

    // The unconditional mark (§6.5 / DEC-044). Licensed by the caller ALREADY
    // holding the whole-node claim — and note the exclusivity that licenses it
    // is a property of the CALLER's state, not of the node: a state word looks
    // identical whether one actor holds all 32 bits or 32 actors hold one each.
    // It can only be asserted from the claim the caller is carrying, never
    // checked at the mark site, which is why there is no "conditional mark" form
    // and must never be one.
    inline void markDying(Atomic<uint64_t>& stateWord) {
        stateWord.fetch_or(state::kMarkMask, kMarkStore);
    }

    // ─── The claim set (§6.1) ──────────────────────────────────────────────
    //
    // "The claim set is the descent stack plus a per-node mask." For every
    // operation except detachment that is bounded by the site count; detachment
    // is bounded instead by the detachment budget, so the capacity is
    // `detachBudget + siteBound` and **the capacity check runs before any entry
    // is written**.
    //
    // Fixed-capacity and stack-resident: never a heap allocation, because a
    // writer may not allocate while holding a claim and because the whole point
    // of DEC-077's budget is that the set is bounded by a constant.
    // `DetachBudget` is a parameter rather than a bare use of `kDetachBudget`
    // because §11's decomposition targets specify "a tiny geometry with a TINY
    // detachBudget" — decomposition is unreachable at the production budget on any
    // tree small enough to assert over. DEC-077 calls 64 "provisional" anyway, so
    // the knob is one the design already expects to turn.
    template <GeometryDescriptor G, unsigned DetachBudget = kDetachBudget>
    struct ClaimSet {
        static constexpr size_t kCapacity = DetachBudget + siteBound(G);

        struct Entry {
            NodeRef  node;
            unsigned level    = 0;
            uint64_t nodeBase = 0;
            uint64_t mask     = 0;
            // A whole-node claim taken because this node is a reclamation
            // candidate or is being detached. Kept distinct from the write-site
            // bits because §6.4 releases a candidacy mask UNUSED when the node
            // turns out not to empty, while write-site bits are held to the end
            // of commit like any claim.
            bool     wholeNode = false;
            // Set once the fetch_or has been issued — the structural half of
            // DEC-046's once-per-node rule.
            bool     issued   = false;
            bool     held     = false;
            uint64_t prior    = 0;
        };

        Entry  entries[kCapacity];
        size_t count = 0;
        // §11's debug counter target: "at most one fetch_or per node per
        // acquisition phase. A second is the self-livelock, which presents as a
        // hang with no output."
        size_t fetchOrIssued = 0;

        void clear() { count = 0; fetchOrIssued = 0; }

        [[nodiscard]] Entry* find(NodeRef n) {
            for (size_t i = 0; i < count; i++) {
                if (entries[i].node == n) return &entries[i];
            }
            return nullptr;
        }

        // Add or MERGE. Merging is not an optimisation: a node that is both a
        // write site and a reclamation candidate — the typical munmap node —
        // must take ONE fetch_or carrying both contributions, because two is
        // DEC-046's deterministic single-CPU self-livelock.
        //
        // Returns false when the set is full, which is the capacity check §6.1
        // requires to run BEFORE any entry is written.
        [[nodiscard]] bool addOrMerge(NodeRef n, unsigned level, uint64_t nodeBase,
                                      uint64_t mask, bool wholeNode) {
            if (Entry* e = find(n)) {
                e->mask |= mask;
                e->wholeNode = e->wholeNode || wholeNode;
                return true;
            }
            if (count >= kCapacity) return false;
            entries[count++] = Entry{n, level, nodeBase, mask, wholeNode, false, false, 0};
            return true;
        }

        // §6.8: claim sites are acquired **top-down — by (depth, then ascending
        // VA), shallowest first**.
        //
        // Depth is a node's level in the CONCEPTUAL FULL-DEPTH TREE, derived
        // from the slot span, not counted as steps during a descent. That is the
        // only formulation invariant under cluster growth: counting steps from
        // the cluster root shifts every node by one when a cluster grows, and
        // counting from the root bucket differs from that by exactly one at all
        // times, so it shifts identically. Two writers straddling a growth would
        // otherwise disagree about the order of the same two nodes — a cycle
        // between two legitimate operations, arising from a bookkeeping choice
        // rather than from any race.
        //
        // Ascending-VA order ALONE is specifically the one that does not work:
        // it is child-before-parent at a range's left edge and parent-before-
        // child at its right, so two disjoint ranges acquire the same pair in
        // opposite orders.
        void sortForAcquisition() {
            // Insertion sort: kCapacity is small and bounded, and this runs once
            // per attempt on a cache-hot array.
            for (size_t i = 1; i < count; i++) {
                Entry e = entries[i];
                size_t j = i;
                while (j > 0 && isAfter(entries[j - 1], e)) {
                    entries[j] = entries[j - 1];
                    j--;
                }
                entries[j] = e;
            }
        }

        static bool isAfter(const Entry& a, const Entry& b) {
            if (a.level != b.level) return a.level > b.level;
            return a.nodeBase > b.nodeBase;
        }
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_CLAIM_H
