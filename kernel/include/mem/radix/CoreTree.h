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

        // ─── Mutation ──────────────────────────────────────────────────────
        //
        // `value == nullptr` clears the range; otherwise it is placed over it.
        // The range is inclusive and must lie within this tree's span (§6.1:
        // an operation never spans clusters, and the policy layer decomposes
        // before reaching here).
        //
        // On OutOfMemory nothing is published and NO reference count has moved:
        // every `+1` is taken at publish, so an abort has nothing to undo.
        [[nodiscard]] ApplyStatus apply(uint64_t lo, uint64_t hi, Mapping* value) {
            assert(rootRef, "radix: apply on an uninitialised tree");
            assert(lo <= hi, "radix: empty operation range");
            assert(contains(lo) && contains(hi),
                   "radix: operation range escapes the cluster — DEC-058 decomposes per "
                   "cluster before the core tree is involved");
            bool becameEmpty = false;
            return applyWithin(rootRef, level0, baseVA, lo, hi, value, becameEmpty);
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

        // ─── The recursive apply ───────────────────────────────────────────
        //
        // Serialized. Phase 2 replaces the traversal with §6.1's two-pass shape
        // (read-only descent computing the claim set, top-down acquisition,
        // re-dispatch, commit boundary) — but every dispatch decision, every
        // reference delta and every occupancy update below is already the one
        // that phase will take.
        static ApplyStatus applyWithin(NodeRef node, unsigned level, uint64_t nodeBase,
                                       uint64_t lo, uint64_t hi, Mapping* value,
                                       bool& nodeBecameEmpty) {
            const uint64_t span = slotSpan(G, level);
            const unsigned n    = valence(G, level);
            const bool writes   = (value != nullptr);

            const unsigned first = static_cast<unsigned>((lo - nodeBase) >> slotSpanBits(G, level));
            const unsigned last  = static_cast<unsigned>((hi - nodeBase) >> slotSpanBits(G, level));
            assert(first < n && last < n, "radix: slot range escapes the node");

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

                case DispatchAction::WriteLeaf:
                    value->acquireRef();                                  // commit-phase +1
                    node.slot(i).store(Codec::encodeLeaf(value, d.range, level), kSlotPublish);
                    bumpOccupancy(node, +1);
                    break;

                case DispatchAction::OverwriteLeaf: {
                    Mapping* displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    value->acquireRef();                                  // +1 before publish
                    node.slot(i).store(Codec::encodeLeaf(value, d.range, level), kSlotPublish);
                    releaseNamedMapping(displaced);                       // deferred -1
                    break;
                }

                case DispatchAction::ShrinkLeafInPlace:
                    // +-0. The slot still names the same record. Deliberately
                    // does not touch the count, and deliberately does not share
                    // a code path with the three rows that do.
                    node.slot(i).store(
                        Codec::encodeLeaf(Codec::decodeLeaf(word), d.range, level), kSlotPublish);
                    break;

                case DispatchAction::ClearSlot: {
                    Mapping* displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    node.slot(i).store(0, kSlotPublish);
                    bumpOccupancy(node, -1);
                    releaseNamedMapping(displaced);
                    break;
                }

                case DispatchAction::DescendIntoChild: {
                    NodeRef child(Codec::decodeChild(word));
                    bool childEmpty = false;
                    const ApplyStatus st = applyWithin(child, level + 1, slotBase,
                                                       clipLo, clipHi, value, childEmpty);
                    if (st != ApplyStatus::Ok) return st;
                    if (childEmpty) {
                        // §6.4: empty-node reclamation is MANDATORY. Without it
                        // the tree grows with cumulative mappings rather than
                        // live ones, and random-probe placement means a freed
                        // region is essentially never reused, so nothing would
                        // ever reclaim by reuse.
                        node.slot(i).store(0, kSlotPublish);
                        bumpOccupancy(node, -1);
                        Ops::destroy(level + 1, child);
                    }
                    break;
                }

                case DispatchAction::DetachChild: {
                    NodeRef child(Codec::decodeChild(word));
                    // Build the replacement BEFORE unlinking anything: nothing
                    // irreversible may precede a step that can still fail.
                    uint64_t replacement = 0;
                    if (writes) {
                        SubRange sub;
                        const bool ok = Codec::subRangeFor(slotBase, level, clipLo, clipHi, sub);
                        assert(ok, "radix: a fully-covered child slot's replacement is the "
                                   "whole span, which is always expressible");
                        (void)ok;
                        value->acquireRef();
                        replacement = Codec::encodeLeaf(value, sub, level);
                    }
                    node.slot(i).store(replacement, kSlotPublish);
                    if (!writes) bumpOccupancy(node, -1);
                    // "A subtree detach is equivalent to clearing every slot and
                    // unlinking every node, executed as one parent-slot store" —
                    // every displaced value then follows the rules an individual
                    // clear already has, and no new lifetime concept appears.
                    releaseSubtree(child, level + 1);
                    break;
                }

                case DispatchAction::Subdivide: {
                    SegmentPlan plan;
                    Mapping* displaced = nullptr;
                    if (Codec::isLeaf(word)) {
                        displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                        for (unsigned s = 0; s < d.survivorCount; s++) {
                            plan.add(d.survivors[s], displaced);
                        }
                    }
                    if (writes) plan.add(d.opRange, value);

                    assert(level < G.levelCount,
                           "radix: subdivision at the deepest level — §6.1's shortfall bound "
                           "says this cannot happen, since the floor expresses every "
                           "page-granular boundary");

                    void* child = buildSubtree(level + 1, slotBase, plan);
                    if (!child) return ApplyStatus::OutOfMemory;

                    // Commit: take every `+1` the new subtree's leaf slots
                    // imply, THEN publish, THEN release the displaced slot's
                    // reference. Ordering matters in exactly one direction — the
                    // increments must precede the decrement, or a subdivision
                    // whose survivors name the record it is displacing could
                    // take the count transiently to zero and destroy it.
                    takeSubtreeReferences(NodeRef(child), level + 1);
                    node.slot(i).store(Codec::encodeChild(child), kSlotPublish);
                    if (Codec::isEmpty(word)) bumpOccupancy(node, +1);
                    if (displaced) releaseNamedMapping(displaced);
                    break;
                }
                }
            }

            nodeBecameEmpty = (occupancyOf(node) == 0);
            return ApplyStatus::Ok;
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
