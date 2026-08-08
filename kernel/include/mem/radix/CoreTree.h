//
// radix-tree — the core tree, Phase 1 (serialized).
//
// One prefix tree over ONE power-of-two-aligned span, parameterized on the
// geometry and on an opaque slot value (DEC-020). The cluster table, placement,
// Mapping policy and the descent cache are the policy layer's and arrive in
// Phase 3; the seam falls here because a cluster table is not a layer above one
// tree's root, it is a CONTAINER OF INDEPENDENT TREES, one per cluster — which
// is what keeps the whole concurrency protocol inside a single core tree
// instead of running it across a seam.
//
// **This file is Phase 1: serialized.** It carries the geometry, the codec, the
// dispatch, descent, the subdivision builder, reclamation and the structural
// halves of the §7 lifetime rules. It carries NO claim protocol, no RCU, no
// retire. Phase 2 wraps the claim/mark/publish/retire protocol of §6 around
// exactly these decisions without changing any of them — which is the point of
// the dispatch being a pure function and of the commit-phase reference rule
// being honoured here even though nothing can fail concurrently yet.
//
// Two rules are implemented in their Phase-2 shape rather than their easy
// serialized shape, deliberately:
//
//   - **Structural `+1`s are taken at publish, never while building** (§7.2 /
//     DEC-067). A subdivision subtree is constructed holding PLANNED slot words
//     and no counted references at all; the references are taken in one walk at
//     the moment the parent slot is published. The easy version — take the
//     reference while building the child — leaks on every abort path: the
//     discarded child is the only record of the increment, nothing undoes it,
//     and the inflated count pins the record, its VMObject and its frames for
//     the address space's life. Invisible to a lifetime test unless it injects
//     a mid-operation abort, which is why Phase 0 built the injector.
//
//   - **A detached subtree is released node by node, each node releasing the
//     Mapping references ITS OWN leaf slots hold** (§7.1 / DEC-047) — never by
//     a recursing destructor. Child-node slots release nothing, because each
//     child is released in its own right; a Mapping has no independent unlink
//     event, so if the parent does not release it, nothing does. The asymmetry
//     is forced, not chosen.
//

#ifndef CROCOS_RADIX_CORE_TREE_H
#define CROCOS_RADIX_CORE_TREE_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <mem/VMSubstrate.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/Ordering.h>
#include <mem/radix/SlotCodec.h>
#include <mem/radix/Node.h>
#include <mem/radix/Mapping.h>
#include <mem/radix/Dispatch.h>
#include <mem/radix/Claim.h>

namespace kernel::mm::radix {

    // ─── Concrete-type dispatch on level (DEC-062) ─────────────────────────
    //
    // The one place a level must become a type. Closed at compile time over the
    // valences the geometry can produce; under the amd64 default this is the
    // two-way branch DEC-062 promises, and RCU-DEC-016's member-pointer
    // `retire<T, &T::head>` is exactly why it has to exist at all.
    template <GeometryDescriptor G>
    struct NodeOps {
        static void* alloc(unsigned level, uint64_t initialRefcount) {
            switch (valence(G, level)) {
#define RADIX_NODE_CASE(V)                                                        \
                case V: {                                                         \
                    auto p = VMSubstrate::tryMake<Node<G, V>>(initialRefcount);   \
                    return p ? p.raw() : nullptr;                                 \
                }
                RADIX_NODE_CASE(2)
                RADIX_NODE_CASE(4)
                RADIX_NODE_CASE(8)
                RADIX_NODE_CASE(16)
                RADIX_NODE_CASE(32)
#undef RADIX_NODE_CASE
                default:
                    assert(false, "radix: no node type for this level's valence — DEC-039 "
                                  "caps valence at 32 and the geometry must be a power of two");
                    return nullptr;
            }
        }

        static void destroy(unsigned level, NodeRef n) {
            switch (valence(G, level)) {
#define RADIX_NODE_CASE(V)                                                        \
                case V:                                                           \
                    VMSubstrate::destroy(VMSubstrate::SafePtr<Node<G, V>>(         \
                        static_cast<Node<G, V>*>(n.raw())));                       \
                    return;
                RADIX_NODE_CASE(2)
                RADIX_NODE_CASE(4)
                RADIX_NODE_CASE(8)
                RADIX_NODE_CASE(16)
                RADIX_NODE_CASE(32)
#undef RADIX_NODE_CASE
                default:
                    assert(false, "radix: no node type for this level's valence");
            }
        }
    };

    enum class ApplyStatus : uint8_t {
        Ok,
        // §6.1: a conflict, or a re-dispatch that changed the claim set. Never
        // surfaced to the caller — `apply` loops on it after releasing
        // everything and backing off. Present as a value so the attempt
        // machinery can say WHY it is unwinding.
        Retry,
        // §6.5 / DEC-077: the operation's summed detachment count exceeds the
        // budget, or its claim set exceeds the fixed capacity. The operation
        // must decompose per child slot at the site node rather than take the
        // unit path — which is not a tuning nicety: the unit path here is an
        // unbounded all-or-nothing acquisition running non-preemptibly inside
        // ONE open read section, i.e. a scheduling-latency defect AND a
        // domain-wide EBR reclamation stall, both reachable by an ordinary
        // MAP_FIXED over a subdivided region.
        NeedsDecomposition,
        // vmsmalloc DEC-048 / radix DEC-075: allocation is failable and every
        // site handles null. §10's inverted hazard is that a site written
        // against never-null `make<T>` re-imports the userspace-triggerable
        // panic through the back door.
        OutOfMemory,
    };

    // The result of a lookup: the record, and the ABSOLUTE range it covers.
    // §3.1's validity token is built from the decoded pair — never from the raw
    // slot word, which is level-relative and compares equal across a shortfall
    // subdivision that halved the covered range.
    struct LookupResult {
        Mapping* mapping = nullptr;
        uint64_t rangeLo = 0;
        uint64_t rangeHi = 0;   // inclusive
        explicit operator bool() const { return mapping != nullptr; }
    };

    template <GeometryDescriptor G, typename Codec>
    class CoreTree {
    public:
        using Ops = NodeOps<G>;

        // A tree rooted at `rootLevel` covering `[base, base + nodeSpan(rootLevel))`.
        // The base is span-aligned by construction: a cluster root is a node of
        // the conceptual full-depth tree, so prefix indexing inside it is free
        // and growth is exact (the old root becomes precisely one child slot of
        // its new parent, so no mapping moves).
        CoreTree() = default;

        [[nodiscard]] bool init(unsigned rootLevel, uint64_t base) {
            assert(rootLevel >= 1 && rootLevel <= G.levelCount,
                   "radix: root level out of range");
            assert(base % nodeSpan(G, rootLevel) == 0,
                   "radix: cluster base must be span-aligned — prefix indexing inside the "
                   "cluster depends on it");
            void* p = Ops::alloc(rootLevel, /*initialRefcount=*/1);
            if (!p) return false;
            rootRef = NodeRef(p);
            level0  = rootLevel;
            baseVA  = base;
            return true;
        }

        // Phase 1 teardown. Phase 3 replaces this with §7.4's unit-decomposed
        // walk; the structural obligation it already honours is that every node
        // releases the Mapping references its own leaf slots hold, and that
        // nothing recurses through a destructor.
        void destroyTree() {
            if (!rootRef) return;
            releaseSubtree(rootRef, level0);
            rootRef = NodeRef();
        }

        [[nodiscard]] bool valid() const { return static_cast<bool>(rootRef); }
        [[nodiscard]] unsigned rootLevel() const { return level0; }
        [[nodiscard]] uint64_t base() const { return baseVA; }
        [[nodiscard]] uint64_t span() const { return nodeSpan(G, level0); }
        [[nodiscard]] NodeRef root() const { return rootRef; }
        [[nodiscard]] bool contains(uint64_t va) const {
            return va >= baseVA && va < baseVA + span();
        }

        // ─── Lookup (§5.5) ─────────────────────────────────────────────────
        //
        // Masked descent: the level is known by counting down from the root's
        // level, and a subtree's base by masking the search key. Nothing is
        // stored in a node to make traversal work — no parent pointers, no level
        // field — which is what frees metadata placement from hot-path
        // considerations (DEC-012) and what makes §6.1's read-only pass natural,
        // since it is already accumulating exactly the descent stack a writer
        // needs.
        [[nodiscard]] LookupResult lookup(uint64_t va) const {
            if (!rootRef || !contains(va)) return {};

            NodeRef node = rootRef;
            unsigned level = level0;
            uint64_t nodeBase = baseVA;

            for (;;) {
                const unsigned idx = slotIndexFor(level, nodeBase, va);
                const uint64_t word = node.slot(idx).load(kSlotLoad);
                const uint64_t slotBase = nodeBase + uint64_t{idx} * slotSpan(G, level);

                switch (Codec::kindOf(word)) {
                case SlotKind::Empty:
                    return {};
                case SlotKind::Leaf: {
                    // The `covers` hook resolves the NEGATIVE answer without
                    // dereferencing the Mapping at all, which is the common
                    // unmapped-VA lookup (DEC-022).
                    if (!Codec::covers(word, va - slotBase, level)) return {};
                    LookupResult r;
                    r.mapping = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    Codec::absoluteRange(word, slotBase, level, r.rangeLo, r.rangeHi);
                    return r;
                }
                case SlotKind::Child:
                    assert(level < G.levelCount,
                           "radix: child pointer at the deepest level — the geometry says "
                           "there is nothing below it");
                    node = NodeRef(Codec::decodeChild(word));
                    nodeBase = slotBase;
                    level++;
                    break;
                }
            }
        }

        // ─── Mutation (§6.1's two-pass shape) ──────────────────────────────
        //
        // Every mutation is:
        //
        //   read-only descent -> allocate -> acquire -> re-dispatch -> |
        //     -> mark -> reference -> publish -> retire -> release
        //
        // The bar is the COMMIT BOUNDARY, crossed exactly once.
        //
        // **Why the descent is read-only and separate.** A writer cannot know it
        // needs a PARENT's claim bit until it has read the CHILD's occupancy,
        // which is deeper. Discovering the set while acquiring it would therefore
        // acquire out of order; claiming every descended-through slot would
        // restore order but would make two disjoint munmaps conflict at every
        // shared ancestor, destroying the disjointness the design exists for.
        // Computing the set first and acquiring it as a unit is ordered by
        // construction.
        //
        // **Nothing irreversible precedes the boundary**, and the MARK is the
        // subtler of the two irreversible steps: a partially-applied mmap at
        // least leaves an observable wrong mapping, whereas a subtree marked by
        // an operation that then backs off leaves no symptom at all until
        // something tries to map that range, forever.
        //
        // **Nothing after the boundary can fail.** Allocation is behind it, every
        // slot to be written is claim-protected, and every mark is under a
        // whole-node claim.
        //
        // `value == nullptr` clears the range; otherwise it is placed over it.
        [[nodiscard]] ApplyStatus apply(uint64_t lo, uint64_t hi, Mapping* value) {
            assert(rootRef, "radix: apply on an uninitialised tree");
            assert(lo <= hi, "radix: empty operation range");
            assert(contains(lo) && contains(hi),
                   "radix: operation range escapes the cluster — DEC-058 decomposes per "
                   "cluster before the core tree is involved");

            unsigned retryCount = 0;
            for (;;) {
                Attempt attempt;
                const ApplyStatus st = runAttempt(lo, hi, value, attempt);
                if (st == ApplyStatus::Retry) {
                    // §9: release EVERYTHING, discard, retry from a fresh read
                    // pass. The claim set is never extended in place — a
                    // re-dispatch that changes it discards and retries, because
                    // re-dispatch can reveal a site ordered BEFORE a held one: a
                    // slot the writer merely descended through is unprotected, so
                    // a concurrent reclamation can empty it and turn it into a
                    // claim site at the PARENT level, same depth as a held bit,
                    // lower VA. Recomputing avoids the inversion rather than
                    // detecting it.
                    abandon(attempt);
                    backoff(++retryCount);
                    continue;
                }
                if (st != ApplyStatus::Ok) abandon(attempt);
                return st;
            }
        }

        // ─── Structural walk, for validators and tests ─────────────────────
        //
        // Visits every (level, slotBase, word) in ascending VA. `fn` returns
        // void. Kept here rather than in the harness because the quiesced-tree
        // validators of §11 need it and because a walk written against the
        // node's real layout is the one that catches a layout regression.
        template <typename F>
        void walk(F&& fn) const {
            if (rootRef) walkNode(rootRef, level0, baseVA, fn);
        }

        // Live node count, for the accounting targets.
        [[nodiscard]] size_t nodeCount() const {
            size_t n = 0;
            if (rootRef) countNodes(rootRef, level0, n);
            return n;
        }

    private:
        NodeRef  rootRef{};
        unsigned level0 = 0;
        uint64_t baseVA = 0;

        static unsigned slotIndexFor(unsigned level, uint64_t nodeBase, uint64_t va) {
            return static_cast<unsigned>(((va - nodeBase) >> slotSpanBits(G, level)) &
                                         (valence(G, level) - 1));
        }

        // ─── Occupancy (§6.3) ──────────────────────────────────────────────
        //
        // Per-node, and it DOES NOT PROPAGATE: a munmap's upward walk stops at
        // the first ancestor that was already non-empty. That stopping property
        // is what distinguishes it from the gap augmentation §5.6 forbids —
        // max-gap is a fine-grained value that changes on every operation and
        // must therefore reach the root, whereas this is a zero/nonzero
        // predicate whose propagation dies at the first nonzero->nonzero
        // transition. Propagating unconditionally would reintroduce exactly the
        // root contention this design exists to avoid.
        static void bumpOccupancy(NodeRef node, int delta) {
            if (delta == 0) return;
            if (delta > 0) {
                const uint64_t prior = node.stateWord().fetch_add(state::kCountUnit, kOccupancyPublish);
                assert(state::countOf(prior) + 1 <= 32,
                       "radix: occupancy count exceeded the valence — over-counting is "
                       "undetectable in release and this assert is the only detector");
                (void)prior;
            } else {
                const uint64_t prior = node.stateWord().fetch_sub(state::kCountUnit, kOccupancyClear);
                assert(state::countOf(prior) > 0,
                       "radix: occupancy underflow — the borrow this catches propagates into "
                       "the spare bits, and with the mark placed above the count instead of "
                       "below it would have cleared a dying mark");
                (void)prior;
            }
        }

        static unsigned occupancyOf(NodeRef node) {
            return state::countOf(node.stateWord().load(kQuiescedRead));
        }

        // ─── Segment plan for a subdivision (§6.3's third row) ─────────────
        //
        // The target state of a slot's span, as up to three disjoint intervals
        // each naming a record. A subdivision realises exactly this and nothing
        // else: every surviving address covered, the operation's range covered
        // iff it writes one, everything else unmapped.
        struct Segment {
            Interval range;
            Mapping* mapping;
        };
        struct SegmentPlan {
            Segment items[3];
            unsigned count = 0;
            void add(Interval r, Mapping* m) {
                if (r.empty() || m == nullptr) return;
                assert(count < 3, "radix: a subdivision plan has at most two survivors "
                                  "plus one new range");
                items[count++] = Segment{r, m};
            }
        };

        // Build a subtree over [base, base + slotSpan(parentLevel)) realising
        // `plan`. Returns null on allocation failure, having destroyed whatever
        // it had built — and having taken NO reference counts, which is what
        // makes the failure path count-clean (§7.2 / DEC-067).
        static void* buildSubtree(unsigned childLevel, uint64_t base, const SegmentPlan& plan) {
            void* raw = Ops::alloc(childLevel, /*initialRefcount=*/0);
            if (!raw) return nullptr;
            NodeRef node(raw);

            const uint64_t childSpan = slotSpan(G, childLevel);
            const unsigned n = valence(G, childLevel);
            unsigned occupancy = 0;

            for (unsigned i = 0; i < n; i++) {
                const uint64_t slotBase = base + uint64_t{i} * childSpan;
                const uint64_t slotEnd  = slotBase + childSpan - 1;

                // Which plan segments intersect this slot, clipped to it.
                SegmentPlan local;
                for (unsigned s = 0; s < plan.count; s++) {
                    const Interval& r = plan.items[s].range;
                    if (r.hi < slotBase || r.lo > slotEnd) continue;
                    local.add(Interval{r.lo > slotBase ? r.lo : slotBase,
                                       r.hi < slotEnd  ? r.hi : slotEnd},
                              plan.items[s].mapping);
                }

                if (local.count == 0) continue;      // stays empty

                if (local.count == 1) {
                    // One record over one contiguous piece of this slot. It
                    // terminates here iff this level can express the piece —
                    // exactly, in both directions. A fully-surviving child slot
                    // is the common instance and becomes a full-span leaf.
                    SubRange sub;
                    if (Codec::subRangeFor(slotBase, childLevel,
                                           local.items[0].range.lo,
                                           local.items[0].range.hi, sub)) {
                        node.slot(i).store(
                            Codec::encodeLeaf(local.items[0].mapping, sub, childLevel),
                            kPrivateInit);
                        occupancy++;
                        continue;
                    }
                }

                // Two records in this slot, or one this level cannot express:
                // recurse on the same rule. This is where the wide-survivor
                // spine comes from — a 4 KiB punch through a 32 MiB C0 leaf
                // walks C1 -> C2 -> C3 and lands ~45 leaves naming the old
                // record, not two.
                assert(childLevel < G.levelCount,
                       "radix: subdivision ran past the resolution floor — the range should "
                       "have been expressible at the floor, since POSIX is page-granular and "
                       "the floor is a page");
                void* grand = buildSubtree(childLevel + 1, slotBase, local);
                if (!grand) {
                    destroyUnpublishedSubtree(node, childLevel);
                    return nullptr;
                }
                node.slot(i).store(Codec::encodeChild(grand), kSlotPublish);
                occupancy++;
            }

            for (unsigned k = 0; k < occupancy; k++) bumpOccupancy(node, +1);
            return raw;
        }

        // Shallow-discard's recursive sibling: destroy a subtree that was built
        // but NEVER PUBLISHED. Safe to walk into children precisely because
        // nothing else can reach it, and count-free because no `+1` was ever
        // taken (DEC-067). Distinct from releaseSubtree, which is the published
        // case and must release the Mapping references.
        static void destroyUnpublishedSubtree(NodeRef node, unsigned level) {
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    destroyUnpublishedSubtree(NodeRef(Codec::decodeChild(w)), level + 1);
                }
                // Leaf slots hold no reference yet — that is the whole point of
                // the commit-phase rule.
            }
            Ops::destroy(level, node);
        }

        // Take the structural `+1`s a built subtree's leaf slots imply, at the
        // moment it is published. One walk, at commit, and never during
        // construction.
        static void takeSubtreeReferences(NodeRef node, unsigned level) {
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isLeaf(w)) {
                    static_cast<Mapping*>(Codec::decodeLeaf(w))->acquireRef();
                } else if (Codec::isChild(w)) {
                    takeSubtreeReferences(NodeRef(Codec::decodeChild(w)), level + 1);
                }
            }
        }

        // The published case: release node by node, each node releasing the
        // Mapping references ITS OWN leaf slots hold, child-node slots releasing
        // nothing (§7.1). Post-order so a node is released after the children
        // whose own releases it must not perform.
        static void releaseSubtree(NodeRef node, unsigned level) {
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    releaseSubtree(NodeRef(Codec::decodeChild(w)), level + 1);
                } else if (Codec::isLeaf(w)) {
                    releaseNamedMapping(static_cast<Mapping*>(Codec::decodeLeaf(w)));
                }
            }
            Ops::destroy(level, node);
        }

        // The counting mechanism owns destruction at zero, not the releaser
        // (§7.1 / RCU-DEC-045).
        static void releaseNamedMapping(Mapping* m) {
            if (m->releaseRef()) {
                VMSubstrate::destroy(VMSubstrate::SafePtr<Mapping>(m));
            }
        }

        // ─── One attempt (§3.2: one attempt is ONE read section) ───────────
        //
        // "The read-only pass, acquisition, re-dispatch and commit all run
        // inside the section that observed the links. Closing the section ENDS
        // the attempt and discards the claim set — which is forced, since the
        // claim set is a set of link-loaded node pointers and those do not
        // survive a close."
        //
        // Phase 2 runs the section on the caller's behalf where a domain is
        // bound; with none bound (the single-threaded Phase-1 tests) the shape is
        // identical minus the guard, which is why those tests are the regression
        // suite for this path.
        struct Attempt {
            ClaimSet<G> claims;

            // Subtrees built during the read pass, before any claim is taken —
            // §6.1's "allocate before you claim". Bounded by 2 per level: only
            // the operation's two EDGE slots can be partially covered at any
            // level, and interior slots are fully covered (overwrite, detach or
            // write), which never subdivides.
            static constexpr size_t kMaxPending = 2 * (G.levelCount + 1);
            struct Pending {
                uint64_t nodeBase = 0;
                unsigned level    = 0;
                unsigned slot     = 0;
                void*    subtree  = nullptr;
            };
            Pending pending[kMaxPending];
            size_t  pendingCount = 0;

            // §6.5's CUMULATIVE budget: summed over every fully-covered subtree
            // the operation would detach, not per subtree. A MAP_FIXED covering
            // several sibling subtrees, each individually small, sums them — the
            // per-subtree reading undersizes the fixed-capacity set by up to a
            // factor of the valence.
            unsigned detachNodes = 0;

            [[nodiscard]] bool addPending(uint64_t base, unsigned level, unsigned slot,
                                          void* subtree) {
                if (pendingCount >= kMaxPending) return false;
                pending[pendingCount++] = Pending{base, level, slot, subtree};
                return true;
            }

            [[nodiscard]] void* takePending(uint64_t base, unsigned level, unsigned slot) {
                for (size_t i = 0; i < pendingCount; i++) {
                    if (pending[i].subtree && pending[i].nodeBase == base &&
                        pending[i].level == level && pending[i].slot == slot) {
                        void* p = pending[i].subtree;
                        pending[i].subtree = nullptr;
                        return p;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] void* peekPending(uint64_t base, unsigned level, unsigned slot) const {
                for (size_t i = 0; i < pendingCount; i++) {
                    if (pending[i].subtree && pending[i].nodeBase == base &&
                        pending[i].level == level && pending[i].slot == slot) {
                        return pending[i].subtree;
                    }
                }
                return nullptr;
            }
        };

        // What the read pass reports upward, so a parent can decide candidacy.
        struct ReadOutcome {
            ApplyStatus status   = ApplyStatus::Ok;
            // The pass PREDICTS this node empties. Advisory: the pass holds no
            // claim, so the count may be stale. Candidacy is a necessary
            // precondition, never the decision — §6.4 recomputes it at the
            // commit boundary from the priors the claims froze.
            bool        willEmpty = false;
        };

        ApplyStatus runAttempt(uint64_t lo, uint64_t hi, Mapping* value, Attempt& a) {
            // ─── Pass 1: read-only ─────────────────────────────────────────
            const ReadOutcome rr = readPass(rootRef, level0, baseVA, lo, hi, value, a);
            if (rr.status != ApplyStatus::Ok) return rr.status;

            // ─── Acquire, top-down (§6.8) ──────────────────────────────────
            a.claims.sortForAcquisition();
            if (!acquireAll(a.claims)) return ApplyStatus::Retry;

            // ─── Re-dispatch (still before the boundary, may still fail) ───
            //
            // Re-run the dispatch at every held site against the now-frozen slot
            // words. A row that changed means the set the pass computed is
            // wrong; the answer is to discard and retry, never to extend the set
            // in place.
            if (!redispatchAgrees(rootRef, level0, baseVA, lo, hi, value, a)) {
                return ApplyStatus::Retry;
            }

            // ─── The commit boundary ───────────────────────────────────────
            //
            // §6.4: the reclamation decision is fixed HERE, from the priors the
            // claims froze — a pure bottom-up computation over data the writer
            // already holds. Nothing in it can fail, and it runs BEFORE the
            // first mark.
            commit(rootRef, level0, baseVA, lo, hi, value, a);

            releaseAll(a.claims);
            return ApplyStatus::Ok;
        }

        // ─── Pass 1 ────────────────────────────────────────────────────────
        ReadOutcome readPass(NodeRef node, unsigned level, uint64_t nodeBase,
                             uint64_t lo, uint64_t hi, Mapping* value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = slotSpan(G, level);
            const unsigned first  = slotIndexFor(level, nodeBase, lo);
            const unsigned last   = slotIndexFor(level, nodeBase, hi);

            uint64_t writeMask   = 0;
            unsigned clearsHere  = 0;

            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                const uint64_t word = node.slot(i).load(kSlotLoad);
                const DispatchResult d =
                    dispatchSlot<G, Codec>(word, level, slotBase, clipLo, clipHi, writes);

                switch (d.action) {
                case DispatchAction::NoOp:
                    break;

                case DispatchAction::DescendIntoChild: {
                    // Descent takes NO claim on this slot. The interlock lives in
                    // the child, which the recursion acquires.
                    NodeRef child(Codec::decodeChild(word));
                    const ReadOutcome sub =
                        readPass(child, level + 1, slotBase, clipLo, clipHi, value, a);
                    if (sub.status != ApplyStatus::Ok) return sub;
                    if (sub.willEmpty) {
                        // The child is predicted to empty, so THIS node will clear
                        // its slot — which makes the slot a write site here and
                        // feeds this node's own candidacy. That upward chain is
                        // exactly why the pass must be separate: the parent's need
                        // is discovered from the child's occupancy, which is
                        // deeper.
                        writeMask |= uint64_t{1} << i;
                        clearsHere++;
                    }
                    break;
                }

                case DispatchAction::DetachChild: {
                    writeMask |= uint64_t{1} << i;
                    if (!writes) clearsHere++;
                    NodeRef child(Codec::decodeChild(word));
                    const ApplyStatus st = planDetachment(child, level + 1, slotBase, a);
                    if (st != ApplyStatus::Ok) return ReadOutcome{st, false};
                    break;
                }

                case DispatchAction::Subdivide: {
                    // Allocate NOW: before any claim is taken, and outside any
                    // held bit. A writer that would need to allocate while
                    // holding a claim releases first, which is why a shortfall
                    // costs a retry rather than an in-place allocation.
                    if (!a.peekPending(nodeBase, level, i)) {
                        SegmentPlan plan;
                        if (Codec::isLeaf(word)) {
                            auto* displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                            for (unsigned k = 0; k < d.survivorCount; k++) {
                                plan.add(d.survivors[k], displaced);
                            }
                        }
                        if (writes) plan.add(d.opRange, value);

                        void* built = buildSubtree(level + 1, slotBase, plan);
                        if (!built) return ReadOutcome{ApplyStatus::OutOfMemory, false};
                        if (!a.addPending(nodeBase, level, i, built)) {
                            destroyUnpublishedSubtree(NodeRef(built), level + 1);
                            return ReadOutcome{ApplyStatus::NeedsDecomposition, false};
                        }
                    }
                    writeMask |= uint64_t{1} << i;
                    break;
                }

                case DispatchAction::WriteLeaf:
                case DispatchAction::OverwriteLeaf:
                case DispatchAction::ShrinkLeafInPlace:
                    writeMask |= uint64_t{1} << i;
                    break;

                case DispatchAction::ClearSlot:
                    writeMask |= uint64_t{1} << i;
                    clearsHere++;
                    break;
                }
            }

            // §6.4 candidacy, from the ADVISORY occupancy read. The pass holds
            // no claim, so this may be stale — which is fine and is why the name
            // says advisory: it is a *necessary precondition*, and the exact
            // decision comes from the fetch_or prior at the commit boundary.
            //
            // The conservative reading — treat EVERY node on the path as a
            // candidate — is wrong and costly: it takes a whole-node claim at
            // every level, so two disjoint munmaps conflict at every shared
            // ancestor, which is the disjointness gone as a contention result
            // with no visible error.
            const unsigned observed =
                state::countOf(node.stateWord().load(kAdvisoryOccupancyLoad));
            const bool isRoot    = (node == rootRef);
            const bool candidate = !isRoot && clearsHere > 0 && observed == clearsHere;

            if (writeMask != 0 || candidate) {
                const uint64_t mask =
                    writeMask | (candidate ? valenceMask(G, level) : uint64_t{0});
                if (!a.claims.addOrMerge(node, level, nodeBase, mask, candidate)) {
                    return ReadOutcome{ApplyStatus::NeedsDecomposition, false};
                }
            }
            return ReadOutcome{ApplyStatus::Ok, candidate};
        }

        // Every node of a fully-covered subtree takes a whole-node claim
        // (§6.5's phase one), and the budget is checked as they accumulate.
        //
        // **Every node in the subtree is marked, not just its root** — and that
        // is load-bearing for the descent cache rather than defence in depth:
        // an unmarked interior node inside a detached subtree reads as fresh,
        // resumes a descent, and silently returns a Mapping for a REMAPPED
        // address. Because the refcount keeps it alive, it does not even crash.
        ApplyStatus planDetachment(NodeRef node, unsigned level, uint64_t nodeBase,
                                   Attempt& a) {
            if (++a.detachNodes > kDetachBudget) return ApplyStatus::NeedsDecomposition;
            if (!a.claims.addOrMerge(node, level, nodeBase, valenceMask(G, level),
                                     /*wholeNode=*/true)) {
                return ApplyStatus::NeedsDecomposition;
            }
            const unsigned n = valence(G, level);
            const uint64_t span = slotSpan(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kSlotLoad);
                if (Codec::isChild(w)) {
                    const ApplyStatus st = planDetachment(NodeRef(Codec::decodeChild(w)),
                                                          level + 1,
                                                          nodeBase + uint64_t{i} * span, a);
                    if (st != ApplyStatus::Ok) return st;
                }
            }
            return ApplyStatus::Ok;
        }

        // ─── Acquisition (§6.8) ────────────────────────────────────────────
        static bool acquireAll(ClaimSet<G>& set) {
            for (size_t i = 0; i < set.count; i++) {
                auto& e = set.entries[i];
                // DEC-046's once-per-node rule, enforced structurally: the set
                // merges by node, so reaching an already-issued entry means the
                // set is malformed.
                assert(!e.issued,
                       "radix: second fetch_or on one node in one acquisition phase — the "
                       "deterministic single-CPU self-livelock (DEC-046)");
                e.issued = true;
                set.fetchOrIssued++;

                const ClaimOutcome oc = tryClaim(e.node.stateWord(), e.mask);
                if (!oc.acquired) return false;
                e.held  = true;
                e.prior = oc.prior;
            }
            return true;
        }

        // §6.4 / §7.4: **a marked node is retired STILL HOLDING its claim bits,
        // and they are never released.** Not incidental — §6.7's progress
        // argument needs every marker to remain a real holder at that site, and
        // the retained bits leave `bitmap == valenceMask && mark set` as a
        // distinctive terminal state a debug validator can assert.
        //
        // Implemented as "drop the entry from the release set", which is also
        // what keeps the release from touching memory that no longer belongs to
        // this attempt: the node has been retired, and once RCU integration
        // lands it is alive but unreachable — either way, releasing into it is
        // wrong. (Found by the Phase 0 oracle as a use-after-poison in
        // releaseAll; the never-release rule was in the spec and not in the
        // code.)
        static void retainClaim(ClaimSet<G>& set, NodeRef node) {
            if (auto* e = set.find(node)) e->held = false;
        }

        static void releaseAll(ClaimSet<G>& set) {
            // Reverse order is not required — release is a fetch_and and the
            // order over HELD claims is what §6.8 constrains, not the release
            // sequence — but it keeps the window in which a partially-released
            // set is visible in the same shape as acquisition.
            for (size_t i = set.count; i-- > 0;) {
                auto& e = set.entries[i];
                if (!e.held) continue;
                releaseClaim(e.node.stateWord(), e.mask);
                e.held = false;
            }
        }

        // ─── Re-dispatch ───────────────────────────────────────────────────
        //
        // Every site is now claim-frozen, so re-running the dispatch is exact.
        // §11: "Re-dispatch is re-run, not re-read" — the check is on the ROW,
        // not on a value comparison, because a slot can change in a way a value
        // comparison notices while the row is unchanged, and vice versa.
        bool redispatchAgrees(NodeRef node, unsigned level, uint64_t nodeBase,
                              uint64_t lo, uint64_t hi, Mapping* value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = slotSpan(G, level);
            const unsigned first  = slotIndexFor(level, nodeBase, lo);
            const unsigned last   = slotIndexFor(level, nodeBase, hi);

            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                const uint64_t word = node.slot(i).load(kClaimedSlotLoad);
                const DispatchResult d =
                    dispatchSlot<G, Codec>(word, level, slotBase, clipLo, clipHi, writes);

                if (d.action == DispatchAction::DescendIntoChild) {
                    if (!redispatchAgrees(NodeRef(Codec::decodeChild(word)), level + 1,
                                          slotBase, clipLo, clipHi, value, a)) {
                        return false;
                    }
                    continue;
                }
                if (d.action == DispatchAction::Subdivide) {
                    // The row still needs a subtree, and it must be the one the
                    // read pass built for exactly this site — a subtree built
                    // against a different survivor set would publish the wrong
                    // coverage.
                    if (!a.peekPending(nodeBase, level, i)) return false;
                }
            }
            return true;
        }

        // ─── Commit (nothing here may fail) ────────────────────────────────
        //
        // Order is §6.1's: ALL marks, then the §7.2 `+1`s, then all publishes,
        // then retires, then releases. An earlier formulation had the count
        // "decremented per actually committed clear, bottom-up", which
        // interleaves publishes with marks and contradicts the phase order; it
        // computes the same answer, since everything is frozen, and the static
        // form is the normative one.
        bool commit(NodeRef node, unsigned level, uint64_t nodeBase,
                    uint64_t lo, uint64_t hi, Mapping* value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = slotSpan(G, level);
            const unsigned first  = slotIndexFor(level, nodeBase, lo);
            const unsigned last   = slotIndexFor(level, nodeBase, hi);

            int occupancyDelta = 0;

            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                const uint64_t word = node.slot(i).load(kClaimedSlotLoad);
                const DispatchResult d =
                    dispatchSlot<G, Codec>(word, level, slotBase, clipLo, clipHi, writes);

                switch (d.action) {
                case DispatchAction::NoOp:
                    break;

                case DispatchAction::DescendIntoChild: {
                    NodeRef child(Codec::decodeChild(word));
                    const bool childEmpty =
                        commit(child, level + 1, slotBase, clipLo, clipHi, value, a);
                    if (childEmpty) {
                        // §6.4: reclamation. The child is marked under the
                        // whole-node claim this attempt already holds, THEN
                        // unlinked. The mark is always set before the unlink that
                        // makes it true, never in a retire callback, and never
                        // cleared.
                        markDying(child.stateWord());
                        retainClaim(a.claims, child);
                        node.slot(i).store(0, kSlotPublish);
                        occupancyDelta--;
                        retireNode(child, level + 1);
                    }
                    break;
                }

                case DispatchAction::WriteLeaf:
                    value->acquireRef();                       // commit-phase +1
                    node.slot(i).store(Codec::encodeLeaf(value, d.range, level), kSlotPublish);
                    occupancyDelta++;
                    break;

                case DispatchAction::OverwriteLeaf: {
                    Mapping* displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    value->acquireRef();                       // +1 BEFORE the publish
                    node.slot(i).store(Codec::encodeLeaf(value, d.range, level), kSlotPublish);
                    releaseNamedMapping(displaced);
                    break;
                }

                case DispatchAction::ShrinkLeafInPlace:
                    // +-0. Deliberately does not touch the count and deliberately
                    // does not share a path with the rows that do.
                    node.slot(i).store(
                        Codec::encodeLeaf(Codec::decodeLeaf(word), d.range, level),
                        kSlotPublish);
                    break;

                case DispatchAction::ClearSlot: {
                    Mapping* displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    node.slot(i).store(0, kSlotPublish);
                    occupancyDelta--;
                    releaseNamedMapping(displaced);
                    break;
                }

                case DispatchAction::DetachChild: {
                    NodeRef child(Codec::decodeChild(word));
                    uint64_t replacement = 0;
                    if (writes) {
                        SubRange sub;
                        const bool ok =
                            Codec::subRangeFor(slotBase, level, clipLo, clipHi, sub);
                        assert(ok, "radix: a fully-covered child slot's replacement is the "
                                   "whole span, which is always expressible");
                        (void)ok;
                        value->acquireRef();
                        replacement = Codec::encodeLeaf(value, sub, level);
                    }
                    // Phase two of §6.5: mark the WHOLE subtree, once it is held
                    // exclusively and nothing can fail. Recursive marking alone
                    // would reproduce the irreversibility trap at scale — a
                    // marker that marks half a subtree and then fails deep has
                    // poisoned every node it touched.
                    markSubtree(child, level + 1, a.claims);
                    // The parent-slot store happens AFTER the marking phase, and
                    // is a publish under a bit claimed earlier — not an
                    // acquisition after a mark. A naive "mark is last" assert
                    // must exempt it explicitly.
                    node.slot(i).store(replacement, kSlotPublish);
                    if (!writes) occupancyDelta--;
                    retireSubtree(child, level + 1);
                    break;
                }

                case DispatchAction::Subdivide: {
                    void* child = a.takePending(nodeBase, level, i);
                    assert(child, "radix: commit reached a subdivision with no prebuilt "
                                  "subtree — re-dispatch should have caught this before the "
                                  "boundary");
                    Mapping* displaced =
                        Codec::isLeaf(word) ? static_cast<Mapping*>(Codec::decodeLeaf(word))
                                            : nullptr;
                    // Take every `+1` the new subtree's leaf slots imply, THEN
                    // publish, THEN release the displaced reference. The
                    // increments must precede the decrement, or a subdivision
                    // whose survivors name the record it is displacing could take
                    // the count transiently to zero and destroy it.
                    takeSubtreeReferences(NodeRef(child), level + 1);
                    node.slot(i).store(Codec::encodeChild(child), kSlotPublish);
                    if (Codec::isEmpty(word)) occupancyDelta++;
                    if (displaced) releaseNamedMapping(displaced);
                    break;
                }
                }
            }

            if (occupancyDelta > 0) {
                for (int k = 0; k < occupancyDelta; k++) bumpOccupancy(node, +1);
            } else if (occupancyDelta < 0) {
                for (int k = 0; k < -occupancyDelta; k++) bumpOccupancy(node, -1);
            }

            // Whether THIS node now empties — the answer the parent's own
            // reclamation decision consumes. Computed after the node's own
            // publishes, which is the bottom-up chaining DEC-064 describes.
            //
            // The cluster root is never a candidate: its parent slot is a bucket
            // word with no state word, so the interlock cannot be acquired there,
            // and reclaiming it anyway would leave that word naming freed memory
            // — an entire zone permanently unmappable, with no crash.
            if (node == rootRef) return false;
            return occupancyOf(node) == 0;
        }

        // Abandon: release every claim and shallow-discard every carried
        // allocation. §7.3: an abandoned operation shallow-discards BEFORE
        // returning, and shallow-discard is the direct destroy of a
        // never-published allocation — reachable by nothing, outside the
        // reclamation protocol.
        //
        // Count-clean by construction: every structural `+1` is a commit-phase
        // step, so an operation that crossed no boundary has taken no increments
        // and there is nothing to undo. The carried subtree holds only PLANNED
        // slot values, never counted references.
        void abandon(Attempt& a) {
            releaseAll(a.claims);
            for (size_t i = 0; i < a.pendingCount; i++) {
                if (a.pending[i].subtree) {
                    destroyUnpublishedSubtree(NodeRef(a.pending[i].subtree),
                                              a.pending[i].level + 1);
                    a.pending[i].subtree = nullptr;
                }
            }
            a.pendingCount = 0;
        }

        // §6.7 property 3: the residual failure shape is timing-aligned mutual
        // abort (livelock), excluded PROBABILISTICALLY rather than
        // deterministically. A probed placement re-probes at a fresh random
        // address; a MAP_FIXED retry takes randomized backoff, and DEC-097
        // records that no starvation bound beyond this is added.
        void backoff(unsigned attempt) {
            // Deterministic in the harness (the seedable source is the test
            // side's, §12); the kernel instance draws from DEC-063's entropy
            // source, which is named out-of-spec prerequisite work.
            const unsigned spins = 1u << (attempt < 8 ? attempt : 8);
            for (unsigned k = 0; k < spins; k++) {
                // A compiler barrier is enough here: the loop exists to spread
                // two writers' retry timing apart, not to synchronize anything.
                __asm__ __volatile__("" ::: "memory");
            }
        }

        // ─── Retire (§7.1) ─────────────────────────────────────────────────
        //
        // **PHASE 2, STEP 1: still synchronous.** The RCU integration — unlink ->
        // retire through the node's inline RetireHead, with the deleter doing
        // the releases at grace-period end — is the next increment. What is
        // already in the Phase-2 shape, and must stay that way, is WHAT gets
        // released and BY WHOM:
        //
        //   - a node releases the Mapping references ITS OWN leaf slots hold;
        //   - child-node slots release nothing, because each child is released
        //     in its own right — releasing them here would double-release;
        //   - nothing recurses through a destructor.
        //
        // That asymmetry is forced, not chosen: a child node has its own unlink
        // event and its own retire, while a Mapping has no independent unlink
        // event in a subtree detach, so if the parent does not release it,
        // nothing does.
        //
        // The synchronous form is a use-after-free the moment a concurrent
        // reader exists — §10 names it as "the NATURAL implementation, since the
        // commit walk already visits every node", which is exactly why it is
        // called out here rather than left to be noticed.
        static void retireNode(NodeRef node, unsigned level) {
            releaseLeafSlots(node, level);
            Ops::destroy(level, node);
        }

        static void retireSubtree(NodeRef node, unsigned level) {
            // Node by node, each through its own head — never one retire for the
            // whole subtree. "A subtree detach is equivalent to clearing every
            // slot and unlinking every node, executed as one parent-slot store":
            // every displaced value then follows the rules an individual clear
            // already has, and no new lifetime concept appears.
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isChild(w)) {
                    retireSubtree(NodeRef(Codec::decodeChild(w)), level + 1);
                }
            }
            retireNode(node, level);
        }

        static void releaseLeafSlots(NodeRef node, unsigned level) {
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isLeaf(w)) {
                    releaseNamedMapping(static_cast<Mapping*>(Codec::decodeLeaf(w)));
                }
            }
        }

        // Phase two of §6.5: mark EVERY node of the subtree, not just its root,
        // and retain each one's claims as it goes.
        //
        // Marking only the root passes almost every check — tree shape, the
        // partition invariant and hole-freedom all still hold. It fails only when
        // a descent-cache entry points at an INTERIOR node of the detached
        // subtree, which then reads as fresh, resumes a descent, and silently
        // returns a Mapping for a remapped address. The refcount keeps it alive,
        // so it does not even crash.
        static void markSubtree(NodeRef node, unsigned level, ClaimSet<G>& set) {
            markDying(node.stateWord());
            retainClaim(set, node);
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isChild(w)) {
                    markSubtree(NodeRef(Codec::decodeChild(w)), level + 1, set);
                }
            }
        }

        template <typename F>
        static void walkNode(NodeRef node, unsigned level, uint64_t nodeBase, F& fn) {
            const unsigned n = valence(G, level);
            const uint64_t span = slotSpan(G, level);
            fn(level, nodeBase, node);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    walkNode(NodeRef(Codec::decodeChild(w)), level + 1,
                             nodeBase + uint64_t{i} * span, fn);
                }
            }
        }

        static void countNodes(NodeRef node, unsigned level, size_t& n) {
            n++;
            const unsigned v = valence(G, level);
            for (unsigned i = 0; i < v; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) countNodes(NodeRef(Codec::decodeChild(w)), level + 1, n);
            }
        }
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_CORE_TREE_H
