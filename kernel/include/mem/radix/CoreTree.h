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
#include <rcu/RCU.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/Ordering.h>
#include <mem/radix/SlotCodec.h>
#include <mem/radix/Node.h>
#include <mem/radix/Mapping.h>
#include <mem/radix/Dispatch.h>
#include <mem/radix/Claim.h>

namespace kernel::mm::radix {

    // The counting mechanism owns destruction at zero — not the releaser
    // (§7.1 / RCU-DEC-045: a deleter may release a counted reference the retired
    // object holds, but must not traverse the referent beyond its refcount word;
    // destruction stays with the counting mechanism).
    inline void releaseMappingRef(Mapping* m) {
        if (m->releaseRef()) {
            VMSubstrate::destroy(VMSubstrate::SafePtr<Mapping>(m));
        }
    }

    // §7.1's deleter: **the retire callback at grace-period end, NOT the
    // destructor at refcount zero.**
    //
    // It releases the references its slots hold to objects that are not
    // themselves separately retired — leaf slots release their Mapping
    // reference, child-node slots release NOTHING — and then performs the
    // ordinary self-release of the node's own inbound structural reference.
    //
    // The leaf/child asymmetry is forced, not chosen: a child node has its own
    // unlink event and its own retire, so releasing it from the parent's deleter
    // would double-release; a Mapping has no independent unlink event in a
    // subtree detach, so if the parent's deleter does not release it, nothing
    // does.
    //
    // And deleter-not-destructor is not cosmetic. A node retired while a foreign
    // CPU's descent cache pins it is destroyed only when that cache turns over,
    // which §7.4 says may never happen — so putting the release in the
    // destructor would pin every Mapping that node named, and transitively their
    // VMObjects and physical frames, behind a residue bound stated in NODES.
    // **A retired-but-cache-pinned node holds no Mapping references.**
    //
    // A free function template rather than a member, because
    // `retire<T, &T::head, Deleter>` takes the deleter as a non-type template
    // parameter of type `void(*)(T*)` — RCU-DEC-016's member-pointer recovery
    // shape. Note it needs only the VALENCE, which the concrete type carries;
    // not the level. That is exactly what lets DEC-062's level->type map
    // collapse to a two-way branch at the retire site.
    template <GeometryDescriptor G, typename Codec, unsigned V>
    void deleteRadixNode(Node<G, V>* n) {
        for (unsigned i = 0; i < V; i++) {
            const uint64_t w = n->slots[i].load(kQuiescedRead);
            if (Codec::isLeaf(w)) {
                releaseMappingRef(static_cast<Mapping*>(Codec::decodeLeaf(w)));
            }
        }
        // The self-release. At zero the counting mechanism destroys.
        const uint64_t prior = n->refcount.fetch_sub(1, kRefcountRelease);
        assert(prior != 0, "radix: node structural refcount released below zero");
        if (prior == 1) {
            thread_fence(kRefcountZeroFence);
            VMSubstrate::destroy(VMSubstrate::SafePtr<Node<G, V>>(n));
        }
    }

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

        // DEC-062's closed enumeration made concrete: `retire<T, &T::head>`
        // needs a concrete T at every site, and the one place a type is
        // discovered at runtime — the per-node retire walk of a detached subtree
        // or of teardown, which visits nodes of mixed levels — dispatches on the
        // node's LEVEL, which every walk already has.
        template <typename Codec>
        static void retire(kernel::rcu::Domain& d, unsigned level, NodeRef n) {
            switch (valence(G, level)) {
#define RADIX_NODE_CASE(V)                                                          \
                case V:                                                             \
                    kernel::rcu::retire<Node<G, V>, &Node<G, V>::head,               \
                                        &deleteRadixNode<G, Codec, V>>(               \
                        d, static_cast<Node<G, V>*>(n.raw()));                       \
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

        // The same deleter, run synchronously. Used ONLY by teardown, where the
        // caller has already established that nothing else can reach the tree —
        // Phase 3 replaces this with §7.4's unit-decomposed walk, whose units go
        // through `retire` like every other unlink path. Running the deleter
        // rather than a bespoke teardown release is what keeps "what a node
        // releases" stated in exactly one place.
        template <typename Codec>
        static void runDeleter(unsigned level, NodeRef n) {
            switch (valence(G, level)) {
#define RADIX_NODE_CASE(V)                                                          \
                case V:                                                             \
                    deleteRadixNode<G, Codec, V>(static_cast<Node<G, V>*>(n.raw()));\
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

    // A lookup returns a **counted reference** to a Mapping together with a
    // validity token, and §3.1 is emphatic that conflating the two is a
    // memory-safety defect:
    //
    //   - The COUNTED REFERENCE guarantees the record stays alive after the
    //     guard is destroyed (DEC-015). That is what makes a blocking round-trip
    //     to a userspace pager possible at all — and it exists because **a
    //     reader must close its section before blocking**: EBR stalls
    //     reclamation domain-wide for as long as any section is open, so a guard
    //     held across a pager IPC halts reclamation for every CPU in the domain.
    //   - The VALIDITY TOKEN — the VA together with the DECODED answer, i.e. the
    //     Mapping identity and the ABSOLUTE covered range — guarantees nothing on
    //     its own. **A counted reference does not keep the address mapped**: a
    //     concurrent partial munmap shrinks a leaf's range with a single slot
    //     write that retires nothing and marks nothing, so no other signal
    //     exists for a blocked thread to observe.
    //
    // The token records the DECODED value, never the raw slot word. A slot
    // word's range field is level-relative, so a C0 leaf with range [0,0]
    // covering 8 KiB and the C1 leaf covering 4 KiB that a shortfall
    // subdivision leaves behind are a BIT-IDENTICAL WORD FOR A HALVED RANGE —
    // comparing raw words compares equal on an address that was just unmapped.
    //
    // Move-only, and it RELEASES on destruction. Making the reference implicit in
    // the type is not decoration: the first version of this returned a raw
    // pointer, and the very first concurrent reader test dereferenced a record
    // whose grace period had ended — reported by the Phase 0 oracle as a
    // use-after-poison, and silent without it.
    class LookupResult {
    public:
        LookupResult() = default;
        LookupResult(Mapping* m, uint64_t lo, uint64_t hi)
            : mappingPtr(m), rangeLo(lo), rangeHi(hi) {}

        LookupResult(const LookupResult&)            = delete;
        LookupResult& operator=(const LookupResult&) = delete;

        LookupResult(LookupResult&& o) noexcept
            : mappingPtr(o.mappingPtr), rangeLo(o.rangeLo), rangeHi(o.rangeHi) {
            o.mappingPtr = nullptr;
        }
        LookupResult& operator=(LookupResult&& o) noexcept {
            if (this != &o) {
                reset();
                mappingPtr = o.mappingPtr; rangeLo = o.rangeLo; rangeHi = o.rangeHi;
                o.mappingPtr = nullptr;
            }
            return *this;
        }

        ~LookupResult() { reset(); }

        [[nodiscard]] Mapping* mapping() const { return mappingPtr; }
        [[nodiscard]] uint64_t lo() const { return rangeLo; }
        [[nodiscard]] uint64_t hi() const { return rangeHi; }   // inclusive
        explicit operator bool() const { return mappingPtr != nullptr; }

    private:
        void reset() {
            if (mappingPtr) {
                releaseMappingRef(mappingPtr);
                mappingPtr = nullptr;
            }
        }
        Mapping* mappingPtr = nullptr;
        uint64_t rangeLo = 0;
        uint64_t rangeHi = 0;
    };

    // §11's counters. Three of the progress targets are stated as COUNTERS
    // rather than asserts, and the distinction is load-bearing: phantom-bit and
    // terminal-mask failures are LEGAL executions, so asserting on them fires on
    // a correct build. §6.7's taxonomy is exact and this is its instrumentation.
    //
    // `conflicts` is additionally the standing regression detector for DEC-085's
    // accepted conditional disjointness: "Under bit-granular claims most such
    // pairs should not conflict AT ALL — a conflict counter near zero is the
    // stronger signal, and a HIGH ONE MEANS THE ONE-CONFLICT-SITE ANALYSIS IS
    // WRONG." Acceptance is instrumented, not silent.
    struct TreeStats {
        Atomic<uint64_t> attempts{0};
        Atomic<uint64_t> completions{0};
        // Retries caused by a failed claim acquisition — the genuine conflicts.
        Atomic<uint64_t> claimConflicts{0};
        // Retries caused by re-dispatch finding a changed row. Counted apart
        // because it is a DIFFERENT phenomenon: the claim set was computed
        // against a tree that has since moved, not a contended bit.
        Atomic<uint64_t> redispatchChanges{0};
        // The longest retry run any single operation needed. §9 accepts that
        // individual starvation is unbounded in principle (DEC-097 adds no bound
        // beyond backoff), so this is a COUNTER and never an assert — but a test
        // that sees thousands is looking at a livelock, not at bad luck.
        Atomic<uint64_t> maxRetries{0};

        void reset() {
            attempts.store(0, kPrivateInit);
            maxRetries.store(0, kPrivateInit);
            completions.store(0, kPrivateInit);
            claimConflicts.store(0, kPrivateInit);
            redispatchChanges.store(0, kPrivateInit);
        }
    };

    template <GeometryDescriptor G, typename Codec>
    class CoreTree {
    public:
        using Ops = NodeOps<G>;

        [[nodiscard]] TreeStats& stats() { return counters; }
        [[nodiscard]] const TreeStats& stats() const { return counters; }

        // A tree rooted at `rootLevel` covering `[base, base + nodeSpan(rootLevel))`.
        // The base is span-aligned by construction: a cluster root is a node of
        // the conceptual full-depth tree, so prefix indexing inside it is free
        // and growth is exact (the old root becomes precisely one child slot of
        // its new parent, so no mapping moves).
        CoreTree() = default;

        // The tree's domain is per-address-space (DEC-072 / RCU-DEC-043). One
        // global domain was rejected on the merits: teardown's no-new-users
        // precondition becomes unsatisfiable at process exit, the per-address-
        // space residue bounds become unreachable, and ONE stalled fault-path
        // reader in any process stalls reclamation for every process.
        [[nodiscard]] bool init(unsigned rootLevel, uint64_t base, kernel::rcu::Domain& d) {
            assert(d.initialized(), "radix: the tree's RCU domain must be initialised first");
            domain = &d;
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

            // §3.1: a descent runs inside ONE ReadGuard on the tree's domain —
            // one section per descent, never one per level (DEC-001). Measured:
            // protect plus a 3-node walk costs ~0.9 ns over a bare section, and
            // the SEQ_CST fence dominates.
            //
            // The counted reference the caller may take from the result must be
            // acquired INSIDE this section (§7.3); the guard's scope is what
            // makes that checkable.
            LookupResult r;
            {
                kernel::rcu::ReadGuard guard(*domain);
                r = descendLocked(va);
            }

            // DEC-060's fault-path pump: tryAdvance AFTER the section closes.
            // Placement is forced, not stylistic — deleters run outside any
            // section (RCU-DEC-038), and a pump inside the descent's section
            // could run deleters while link-loaded pointers are still live.
            //
            // The fault path is chosen because it is the one path that runs on
            // every CPU that touches the tree, READERS INCLUDED: a CPU that
            // stops mutating but keeps faulting pumps its own bag out. Without
            // it, a CPU that performs the last munmap and never retires again
            // strands its open bag indefinitely, and an open bag pins one
            // operation's retirees — transitively their Mappings, VMObjects and
            // frames, not merely nodes.
            (void)kernel::rcu::tryAdvance(*domain);
            return r;
        }

        [[nodiscard]] LookupResult descendLocked(uint64_t va) const {
            NodeRef node = rootRef;
            unsigned level = level0;
            uint64_t nodeBase = baseVA;

            for (;;) {
                const unsigned idx = slotIndexFor(level, nodeBase, va);
                // Invariant 21: EVERY link load on a reader path goes through
                // the protected-link-load primitive — protectWord supplies the
                // framework's named ACQUIRE and the debug section-open assert,
                // and the codec supplies the decode. That composite is this
                // consumer's RcuPtr (DEC-073); there is no exception.
                const uint64_t word = kernel::rcu::protectWord(*domain, node.slot(idx));
                const uint64_t slotBase = nodeBase + uint64_t{idx} * slotSpan(G, level);

                switch (Codec::kindOf(word)) {
                case SlotKind::Empty:
                    return {};
                case SlotKind::Leaf: {
                    // The `covers` hook resolves the NEGATIVE answer without
                    // dereferencing the Mapping at all, which is the common
                    // unmapped-VA lookup (DEC-022).
                    if (!Codec::covers(word, va - slotBase, level)) return {};
                    auto* m = static_cast<Mapping*>(Codec::decodeLeaf(word));
                    // §7.3's acquisition law: a counted reference is acquired
                    // ONLY inside the read section in which that link was
                    // observed. Taking it after the close would let the node be
                    // unlinked, retired, graced, released to zero and destroyed,
                    // and its slab slot handed to an unrelated allocation, before
                    // the fetch_add landed on a stranger.
                    m->acquireRef();
                    uint64_t lo = 0, hi = 0;
                    Codec::absoluteRange(word, slotBase, level, lo, hi);
                    return LookupResult{m, lo, hi};
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
            bool retiredAnything = false;
            for (;;) {
                Attempt attempt;
                ApplyStatus st;
                {
                    // §3.2 / invariant 17: **one attempt is one read section.**
                    // The read-only pass, acquisition, re-dispatch and commit all
                    // run inside the section that observed the links; closing it
                    // ENDS the attempt and discards the claim set, which is
                    // forced rather than chosen — the claim set is a set of
                    // link-loaded node pointers and those do not survive a close
                    // (§7.3).
                    //
                    // The writer is inside a section for its OWN safety, not only
                    // for reclamation (RCU-DEC-019): it traverses the shared
                    // structure, so an unpinned writer can have a node reclaimed
                    // underneath it — a use-after-free in the WRITER.
                    kernel::rcu::ReadGuard guard(*domain);
                    counters.attempts.fetch_add(1, kRefcountAcquire);
                    st = runAttempt(lo, hi, value, attempt);
                }
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
                    retiredAnything = retiredAnything || attempt.retired;
                    abandon(attempt);
                    ++retryCount;
                    for (;;) {
                        const uint64_t seen = counters.maxRetries.load(kQuiescedRead);
                        if (seen >= retryCount) break;
                        uint64_t expected = seen;
                        if (counters.maxRetries.compare_exchange(
                                expected, retryCount, kRefcountRelease, kQuiescedRead)) break;
                    }
                    // §9 accepts unbounded individual starvation in principle
                    // (DEC-097 adds no bound beyond backoff), so this ceiling is
                    // DEBUG-ONLY and deliberately absurd: no legal execution
                    // reaches it, and reaching it means a livelock — which
                    // otherwise presents as a hang with no output, the single
                    // worst diagnostic outcome in this protocol (§10 names the
                    // same shape for DEC-046's self-livelock).
                    assert(retryCount < 1000000u,
                           "radix: operation exceeded one million retries — this is a "
                           "livelock, not starvation");
                    backoff(retryCount);
                    continue;
                }
                retiredAnything = retiredAnything || attempt.retired;
                if (st != ApplyStatus::Ok) abandon(attempt);

                // DEC-060/DEC-071's operation-exit pump, AFTER the section
                // closes. It advances the epoch where possible so the
                // just-filled bag stops being the open one and becomes drainable
                // by any CPU's later pump (stealing), rather than sitting open on
                // a CPU that may never mutate this domain again. In honest units
                // that narrows the exposure from "one operation's retirees,
                // forever, until teardown" to "one operation's retirees, until
                // the next pump anywhere".
                if (retiredAnything) (void)kernel::rcu::tryAdvance(*domain);
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
        kernel::rcu::Domain* domain = nullptr;
        TreeStats counters{};
        // Jitter state for the retry backoff. Shared rather than per-CPU because
        // it only needs to decorrelate, and the CPU id is mixed in at use.
        Atomic<uint64_t> backoffSequence{0x243F6A8885A308D3ull};
        // DEC-060: whether this operation retired anything, so operation exit
        // knows whether to pump. Pumping unconditionally would be harmless but
        // dishonest — the site exists to stop a just-filled bag sitting OPEN on
        // a CPU that may never mutate this domain again, and an operation that
        // retired nothing has no bag to advance.
        bool retiredThisOperation = false;

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
        // §7.2: every structural `+1` — on a Mapping OR ON A NODE BEING LINKED
        // — is a commit-phase step. This walk takes all of them for a subtree at
        // the instant its root is published, and never during construction.
        //
        // A node gets exactly one: the slot that names it. The subtree root's
        // comes from the parent slot about to be stored; each interior node's
        // from its own parent slot inside the subtree. So it is +1 per node,
        // plus +1 per naming leaf slot on the Mapping it names.
        static void takeSubtreeReferences(NodeRef node, unsigned level) {
            node.refcount().fetch_add(1, kRefcountAcquire);
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
        // Teardown's release, node by node, each running the SAME deleter the
        // retire path runs — leaf slots release their Mapping reference,
        // child-node slots release nothing, and the node self-releases to zero.
        // Post-order so a node is released after the children whose own releases
        // it must not perform.
        //
        // Phase 3 replaces this with §7.4's unit-decomposed walk (claim, mark,
        // unlink, retire, one read section per unit). What is already right here
        // is that nothing recurses through a destructor and that the release
        // rules are the deleter's, stated once.
        static void releaseSubtree(NodeRef node, unsigned level) {
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    releaseSubtree(NodeRef(Codec::decodeChild(w)), level + 1);
                }
            }
            Ops::template runDeleter<Codec>(level, node);
        }

        // The counting mechanism owns destruction at zero, not the releaser
        // (§7.1 / RCU-DEC-045).
        static void releaseNamedMapping(Mapping* m) { releaseMappingRef(m); }

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
                // The slot word the subtree was BUILT FROM.
                //
                // The read pass runs unclaimed, so between reading a slot and
                // claiming it another writer can replace its contents. The
                // dispatch ROW can be unchanged across that — leaf-partially-
                // covered before and after — while the leaf now names a
                // DIFFERENT record, and the prebuilt subtree still carries
                // survivors naming the old one. Publishing it would map the
                // wrong record, and taking its references would touch a record
                // whose count may already have reached zero.
                //
                // Comparing the raw word is exactly right HERE, and only here:
                // same slot, same level, so identical bits mean identical
                // decoded meaning. (§3.1's validity token cannot do this — it
                // compares ACROSS levels, where a level-relative range field
                // makes a bit-identical word mean a halved range.)
                uint64_t builtFrom = 0;
            };
            Pending pending[kMaxPending];
            size_t  pendingCount = 0;

            // §6.5's CUMULATIVE budget: summed over every fully-covered subtree
            // the operation would detach, not per subtree. A MAP_FIXED covering
            // several sibling subtrees, each individually small, sums them — the
            // per-subtree reading undersizes the fixed-capacity set by up to a
            // factor of the valence.
            unsigned detachNodes = 0;

            // DEC-060: whether this attempt retired anything, so operation exit
            // knows whether to pump. Per-ATTEMPT, not per-tree: a tree is shared
            // by every CPU, so a plain bool member is a genuine data race — TSan
            // caught exactly that, with two writers racing between `apply` and
            // `retireSubtree`. Pumping unconditionally would hide it, and would
            // also be dishonest: the site exists to stop a just-filled bag
            // sitting OPEN on a CPU that may never mutate this domain again, and
            // an operation that retired nothing has no bag to advance.
            bool retired = false;

            [[nodiscard]] bool addPending(uint64_t base, unsigned level, unsigned slot,
                                          void* subtree, uint64_t builtFrom) {
                if (pendingCount >= kMaxPending) return false;
                pending[pendingCount++] = Pending{base, level, slot, subtree, builtFrom};
                return true;
            }

            [[nodiscard]] void* takePending(uint64_t base, unsigned level, unsigned slot,
                                            uint64_t currentWord) {
                for (size_t i = 0; i < pendingCount; i++) {
                    if (pending[i].subtree && pending[i].nodeBase == base &&
                        pending[i].level == level && pending[i].slot == slot &&
                        pending[i].builtFrom == currentWord) {
                        void* p = pending[i].subtree;
                        pending[i].subtree = nullptr;
                        return p;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] void* peekPending(uint64_t base, unsigned level, unsigned slot,
                                            uint64_t currentWord) const {
                for (size_t i = 0; i < pendingCount; i++) {
                    if (pending[i].subtree && pending[i].nodeBase == base &&
                        pending[i].level == level && pending[i].slot == slot &&
                        pending[i].builtFrom == currentWord) {
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
            if (!acquireAll(a.claims)) {
                counters.claimConflicts.fetch_add(1, kRefcountAcquire);
                return ApplyStatus::Retry;
            }

            // ─── Re-dispatch (still before the boundary, may still fail) ───
            //
            // Re-run the dispatch at every held site against the now-frozen slot
            // words. A row that changed means the set the pass computed is
            // wrong; the answer is to discard and retry, never to extend the set
            // in place.
            if (!redispatchAgrees(rootRef, level0, baseVA, lo, hi, value, a)) {
                counters.redispatchChanges.fetch_add(1, kRefcountAcquire);
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

            // §9: "Surplus allocations are shallow-discarded."
            //
            // A prebuilt subtree can go unused on the SUCCESS path, not only on
            // an abort: the read pass runs unclaimed, so a slot it classified as
            // Subdivide can, by the time the claim lands, dispatch to a different
            // row entirely — an overwrite, a clear, a descend. Re-dispatch does
            // not reject that (the operation is still perfectly well-defined), it
            // simply never asks for the subtree. Without this the node leaks, and
            // it leaks only under concurrency, which is where it is hardest to
            // attribute — it surfaced as a single 288 B node outstanding after a
            // four-CPU disjoint-writer run.
            discardUnusedAllocations(a);

            counters.completions.fetch_add(1, kRefcountAcquire);
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
                    if (!a.peekPending(nodeBase, level, i, word)) {
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
                        if (!a.addPending(nodeBase, level, i, built, word)) {
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
        // Whether this attempt holds the WHOLE-NODE claim on `node` — i.e. it
        // was a §6.4 reclamation candidate and its claim landed.
        //
        // Load-bearing, and its absence was a real stall. `commit` computes
        // whether a node empties from its own frozen counts, and that answer can
        // be YES for a node the read pass did NOT nominate as a candidate: the
        // pass reads occupancy with a RELAXED, advisory load and may see a stale,
        // higher count. Reclaiming on that basis marks a node this attempt does
        // not hold — violating invariant 19 ("every mark is taken under a
        // whole-node claim, on all three paths, no exemption") and, because a
        // mark is irreversible and blocks every future claim, leaving a node
        // permanently marked AND STILL LINKED. §6.8 describes the consequence
        // exactly: "every inserter retries from the parent, and the range becomes
        // silently unmappable for the address space's life."
        //
        // §6.4's rule is the other half: "A node that empties but was NOT a
        // candidate is simply not reclaimed on this pass — an opportunistic miss,
        // bounded by the residue figure rather than unbounded in history."
        static bool holdsWholeNode(ClaimSet<G>& set, NodeRef node, unsigned level) {
            const auto* e = set.find(node);
            return e != nullptr && e->held && (e->mask & valenceMask(G, level)) ==
                                                  valenceMask(G, level);
        }

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
                if (d.action == DispatchAction::DetachChild) {
                    // §6.5's FREEZE-AND-VERIFY re-walk, and it is phase one's
                    // last step rather than a nicety.
                    //
                    // The claim set was recorded by the UNCLAIMED read pass, and
                    // a concurrent placement can legally build nodes into a hole
                    // of the subtree between that enumeration and the claims
                    // landing — §6.4 calls exactly this interloper "disjoint and
                    // entirely legal". Once every recorded node is whole-node-
                    // claimed the subtree is frozen, so re-enumerating it under
                    // the claims and requiring the discovered set to equal the
                    // recorded one is what makes phase two safe.
                    //
                    // Without it the marks orphan the unclaimed newcomers
                    // UNMARKED AND LIVE — the DEC-016 cache hazard — and their
                    // leaf Mapping references leak. Worse in practice: marking a
                    // node this attempt does not hold leaves it permanently
                    // marked and still linked, and every later writer stalls on
                    // it forever.
                    if (!detachmentIsFrozen(NodeRef(Codec::decodeChild(word)),
                                            level + 1, a.claims)) {
                        return false;
                    }
                    continue;
                }
                if (d.action == DispatchAction::Subdivide) {
                    // The row still needs a subtree, and it must be the one the
                    // read pass built for exactly this site — a subtree built
                    // against a different survivor set would publish the wrong
                    // coverage.
                    // Not merely "a subtree exists" but "the subtree built for
                    // exactly THIS content". A row that is still Subdivide over a
                    // slot whose leaf now names a different record needs a
                    // different subtree, and reusing the old one publishes the
                    // wrong coverage.
                    if (!a.peekPending(nodeBase, level, i, word)) return false;
                }
            }
            return true;
        }

        // Every node of the subtree AS IT IS NOW must be one this attempt holds
        // a whole-node claim on. Any extra node means the subtree grew under us
        // between the read pass and the claims: release everything and retry
        // from a fresh read pass (which re-counts, and decomposes if the subtree
        // is now over budget).
        static bool detachmentIsFrozen(NodeRef node, unsigned level, ClaimSet<G>& set) {
            if (!holdsWholeNode(set, node, level)) return false;
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isChild(w)) {
                    if (!detachmentIsFrozen(NodeRef(Codec::decodeChild(w)), level + 1, set)) {
                        return false;
                    }
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
                    // Candidacy is a NECESSARY PRECONDITION, not the decision.
                    // The exact emptiness answer comes from the claim-frozen
                    // counts above; what this adds is that we may only ACT on it
                    // where the attempt actually holds the interlock.
                    if (childEmpty && holdsWholeNode(a.claims, child, level + 1)) {
                        // §6.4: reclamation. The child is marked under the
                        // whole-node claim this attempt already holds, THEN
                        // unlinked. The mark is always set before the unlink that
                        // makes it true, never in a retire callback, and never
                        // cleared.
                        markDying(child.stateWord());
                        retainClaim(a.claims, child);
                        node.slot(i).store(0, kSlotPublish);
                        occupancyDelta--;
                        retireNode(child, level + 1, a);
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
                    retireSubtree(child, level + 1, a);
                    break;
                }

                case DispatchAction::Subdivide: {
                    void* child = a.takePending(nodeBase, level, i, word);
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
            discardUnusedAllocations(a);
        }

        // Shallow-discard every carried allocation the attempt did not publish.
        // "Shallow-discard is the direct destroy of a never-published allocation
        // — reachable by nothing, outside the reclamation protocol" (§7.3), and
        // it is count-clean by construction: every structural `+1` is a
        // commit-phase step taken at publish, so an unpublished subtree holds
        // only PLANNED slot values and never a counted reference.
        static void discardUnusedAllocations(Attempt& a) {
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
        // abort (livelock), and it is excluded **PROBABILISTICALLY rather than
        // deterministically**. A probed placement re-probes at a fresh random
        // address; a MAP_FIXED retry takes RANDOMIZED backoff, and DEC-097
        // records that no starvation bound beyond that is added.
        //
        // **The randomization is load-bearing, not decoration.** A deterministic
        // exponential backoff leaves threads that started together still aligned
        // after every doubling, so they re-collide at each retry — which is
        // precisely the timing-aligned mutual abort the randomization exists to
        // break. Measured here: with a deterministic `1 << attempt` spin, four
        // CPUs contending on one node stalled indefinitely (~100 of 1200
        // operations completing before the test timed out); two CPUs completed
        // fine, which is the signature of alignment rather than of deadlock.
        //
        // The jitter source is deliberately cheap and local rather than drawn
        // from an entropy service: DEC-063's in-kernel CSPRNG is named out-of-
        // spec prerequisite work for Phase 3, and backoff jitter does not need a
        // cryptographic source — only decorrelation between CPUs. Mixing the
        // CPU id in is what supplies that.
        void backoff(unsigned attempt) {
            const unsigned capped = attempt < 10 ? attempt : 10;
            const uint64_t window = uint64_t{1} << capped;

            // splitmix64 step over (cpu, attempt, a per-tree sequence). Distinct
            // per CPU by construction, so two CPUs at the same retry index draw
            // different delays.
            uint64_t x = backoffSequence.fetch_add(0x9E3779B97F4A7C15ull, kRefcountAcquire);
            x += static_cast<uint64_t>(arch::getCurrentProcessorID()) * 0xD1B54A32D192ED03ull;
            x += uint64_t{attempt} * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            x ^= x >> 31;

            const uint64_t spins = 1 + (x & (window - 1));
            for (uint64_t k = 0; k < spins; k++) {
                // A compiler barrier is enough: the loop exists to spread two
                // writers' retry timing apart, not to synchronize anything.
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
        void retireNode(NodeRef node, unsigned level, Attempt& a) {
            // Unlink -> retire. The unlink store has already happened at the
            // call site: rcu.md's writer obligation is that EVERY store making
            // the object unreachable is sequenced before `retire`, and `retire`
            // issues its own fence.
            Ops::template retire<Codec>(*domain, level, node);
            a.retired = true;
        }

        void retireSubtree(NodeRef node, unsigned level, Attempt& a) {
            // Node by node, each through its own head — never one retire for the
            // whole subtree. "A subtree detach is equivalent to clearing every
            // slot and unlinking every node, executed as one parent-slot store":
            // every displaced value then follows the rules an individual clear
            // already has, and no new lifetime concept appears.
            const unsigned n = valence(G, level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isChild(w)) {
                    retireSubtree(NodeRef(Codec::decodeChild(w)), level + 1, a);
                }
            }
            retireNode(node, level, a);
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
