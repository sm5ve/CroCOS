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
#include <interrupts/InterruptContextDepths.h>   // the record-draw context assert

#include <mem/radix/Geometry.h>
#include <mem/radix/Ordering.h>
#include <mem/radix/ProgressAudit.h>
#include <mem/radix/SlotCodec.h>
#include <mem/radix/Node.h>
#include <mem/radix/Mapping.h>
#include <mem/radix/DeferredRelease.h>
#include <mem/radix/BucketCodec.h>
#include <mem/radix/Dispatch.h>
#include <mem/radix/Claim.h>

namespace kernel::mm::radix {

    // `releaseMappingRef` / `releaseMappingRefs` live in Mapping.h — the
    // DeferredRelease deleter needs them and this header is downstream of it.

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
    // The ONE destroy site. Every path that frees a node — the deleter's
    // zero-observing self-release, the shallow-discard of an unpublished
    // allocation, and the descent cache's eviction release — goes through here,
    // which is what makes the Phase 5 node census a pair of counters rather than
    // a set of them that have to be kept in agreement.
    template <GeometryDescriptor G, unsigned V>
    void destroyNode(Node<G, V>* n) {
        noteNodeDestroyed();
        VMSubstrate::destroy(VMSubstrate::SafePtr<Node<G, V>>(n));
    }

    template <GeometryDescriptor G, typename Codec, unsigned V>
    void deleteRadixNode(Node<G, V>* n) {
        // ─── What onPreTouch DOES cover, corrected (R-6) ────────────────
        //
        // This used to say `onPreTouch` covered the `RetireHead` "and NOTHING
        // else, so the slot reads below and the refcount RMW are cross-CPU
        // accesses". At byte granularity that is true; at the granularity that
        // exists it is not. **Freshness is PAGE-granular**, and a node is 160 or
        // 288 B inside one page, so the hook that ran before this deleter already
        // covered the whole node body.
        //
        // So this `SafePtr` is UNIFORMITY, not necessity — every cross-CPU pointer
        // in this subsystem is wrapped, and carving out an exception whose
        // justification is an allocator's size classes would be a worse trade than
        // one redundant call. Do not read it as load-bearing: deleting it is
        // undetectable by construction, which is how R-6 was filed as a coverage
        // gap when it was really a wrong comment.
        //
        // **The load-bearing call is the one on the `Mapping` below**, which is a
        // separate allocation on a separate page that `onPreTouch` never touched.
        // That one is asserted by
        // `radix_node_deleter_discharges_freshness_on_the_retired_node`.
        //
        // Deleters run wherever a pump runs, which under RCU-DEC-006 stealing is
        // usually not the CPU that allocated anything involved.
        const VMSubstrate::SafePtr<Node<G, V>> node(n);
        for (unsigned i = 0; i < V; i++) {
            const uint64_t w = node->slots[i].load(kQuiescedRead);
            if (Codec::isLeaf(w)) {
                // Each `Mapping` is its own allocation on its own page: this
                // node being fresh says nothing about the records it names, so
                // the SafePtr is rebuilt rather than borrowed.
                releaseMappingRef(VMSubstrate::SafePtr<Mapping>(
                    static_cast<Mapping*>(Codec::decodeLeaf(w))));
            }
        }
        // The self-release. At zero the counting mechanism destroys.
        // ACQ_REL rather than DEC-069's release-plus-acquire-fence, for the
        // reason spelled out at kRefcountReleaseAcquire: the fence form is
        // correct and invisible to the release gate (D-029).
        const uint64_t prior = node->refcount.fetch_sub(1, kRefcountReleaseAcquire);
        assert(prior != 0, "radix: node structural refcount released below zero");
        if (prior == 1) destroyNode<G, V>(n);
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
            switch (Geo<G>::valence(level)) {
#define RADIX_NODE_CASE(V)                                                        \
                case V: {                                                         \
                    auto p = VMSubstrate::tryMake<Node<G, V>>(initialRefcount);   \
                    if (!p) return nullptr;                                       \
                    noteNodeAllocated();                                          \
                    return p.raw();                                               \
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
            switch (Geo<G>::valence(level)) {
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
            switch (Geo<G>::valence(level)) {
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
            switch (Geo<G>::valence(level)) {
#define RADIX_NODE_CASE(V)                                                        \
                case V:                                                           \
                    destroyNode<G, V>(static_cast<Node<G, V>*>(n.raw()));          \
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

    // ─── The root binding (§5.1, §7.3; DEC-103) ────────────────────────────
    //
    // A cluster's root pointer, its root LEVEL and its base are three values
    // that move together — growth allocates a node above the current root, so
    // all three change at once — and DEC-103 packs them into one bucket word for
    // exactly that reason. What the tree needs from them is stated in §7.3 and
    // is narrower than it first looks:
    //
    //   - **Decode all three from ONE load.** The failure mode is not staleness:
    //     growth does not relabel the old root, so a stale triple stays a TRUE
    //     description of the node it names, and an operation running against it
    //     is an ordinary RCU snapshot. The failure is pairing a FRESH pointer
    //     with a STALE level and base — the writer then computes bit indices for
    //     a node that is not shaped that way, "on a node it holds a valid claim
    //     in". Packing makes that impossible per load; using the load atomically
    //     is what makes it impossible in the code.
    //
    //   - **Do not carry the root pointer across a section close.** It is
    //     uncounted (§7.3), and this has teeth precisely because of growth:
    //     before growth the cluster root is exempt from §6.4's reclamation walk,
    //     but after growth it is an ordinary interior node with a parent slot
    //     and a state word, so it becomes reclaimable the moment it empties.
    //
    // Hence one decode per section, at the top of the work that section does.
    struct RootBinding {
        NodeRef  root{};
        unsigned level = 0;
        uint64_t base  = 0;

        explicit operator bool() const { return static_cast<bool>(root); }
    };

    // §5.6/DEC-007: "The verify pass IS the install pass, so a successful `mmap`
    // costs one traversal." That fusion is a CORRECTNESS property before it is a
    // performance one. A placement that verifies emptiness, then installs in a
    // second traversal, can have a concurrent placement take the range in
    // between — and the second install silently overwrites it, losing a mapping
    // with no error anywhere. `OnlyIfEmpty` moves the verification inside the
    // attempt, where the claims make it exact.
    enum class ApplyMode : uint8_t {
        // MAP_FIXED: whatever is there is displaced.
        Overwrite,
        // Probed placement: the operation fails as `Occupied` if any part of the
        // range is already mapped, judged under the attempt's own claims.
        OnlyIfEmpty,
    };

    enum class ApplyStatus : uint8_t {
        Ok,
        // §6.1: a conflict, or a re-dispatch that changed the claim set. Never
        // surfaced to the caller — `apply` loops on it after releasing
        // everything and backing off. Present as a value so the attempt
        // machinery can say WHY it is unwinding.
        Retry,
        // §6.5 / DEC-077: the operation's summed detachment count exceeds the
        // budget, its claim set exceeds the fixed capacity, or (D-059) its
        // deferred-release demand exceeds the record budget. The operation
        // must decompose per child slot at the site node rather than take the
        // unit path — which is not a tuning nicety: the unit path here is an
        // unbounded all-or-nothing acquisition running non-preemptibly inside
        // ONE open read section, i.e. a scheduling-latency defect AND a
        // domain-wide EBR reclamation stall, both reachable by an ordinary
        // MAP_FIXED over a subdivided region.
        //
        // The three causes share one status deliberately: they are three
        // different resources whose exhaustion has the same cure, and each
        // decomposition step reduces all three at once.
        NeedsDecomposition,
        // vmsmalloc DEC-048 / radix DEC-075: allocation is failable and every
        // site handles null. §10's inverted hazard is that a site written
        // against never-null `make<T>` re-imports the userspace-triggerable
        // panic through the back door.
        OutOfMemory,
        // §7.1 / DEC-068: this CPU's DeferredRelease pool was momentarily EMPTY.
        // Never surfaced to the caller and NOT a failure — the records are in
        // flight through this CPU's own earlier retires, and `barrier` brings
        // them home deterministically. A distinct value from `Retry` because the
        // remedy is a blocking call that must run between attempts and outside
        // any section; retrying without it just re-fails.
        //
        // Note the narrowness: "the pool is empty right now", never "the attempt
        // wants more than the pool holds". The second is `NeedsDecomposition`,
        // and D-059 is the record of what happens when the two are conflated —
        // no grace period can satisfy a demand the population cannot hold, so
        // the replenish loop spins.
        NeedsRecords,
        // `OnlyIfEmpty` found the range occupied. A normal placement outcome —
        // the probe moves on — and never a failure.
        Occupied,
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
        using Ref = VMSubstrate::SafePtr<Mapping>;

        LookupResult() = default;
        LookupResult(Ref m, uint64_t va, uint64_t lo, uint64_t hi)
            : mappingPtr(m), searchVA(va), rangeLo(lo), rangeHi(hi) {}

        LookupResult(const LookupResult&)            = delete;
        LookupResult& operator=(const LookupResult&) = delete;

        LookupResult(LookupResult&& o) noexcept
            : mappingPtr(o.mappingPtr), searchVA(o.searchVA),
              rangeLo(o.rangeLo), rangeHi(o.rangeHi) {
            o.mappingPtr = Ref{};
        }
        LookupResult& operator=(LookupResult&& o) noexcept {
            if (this != &o) {
                reset();
                mappingPtr = o.mappingPtr; searchVA = o.searchVA;
                rangeLo = o.rangeLo; rangeHi = o.rangeHi;
                o.mappingPtr = Ref{};
            }
            return *this;
        }

        ~LookupResult() { reset(); }

        // ─── Why this returns a SafePtr and not a `Mapping*` ───────────────
        //
        // DEC-015 exists so the fault path can close its section and block on a
        // userspace pager, which means this object is DESIGNED to outlive a
        // section and a round-trip — and therefore to be used on a CPU that is
        // not the one that looked it up. Freshness is a property of one CPU's
        // mapping, so a call made at acquisition guarantees nothing to whoever
        // resumes: that CPU may hold a stale entry for the record's page from a
        // previous tenant. Handing out a raw pointer here made the caller's
        // `baseVA` and `objectOffset` reads unguarded on exactly the path the
        // whole feature was built around.
        //
        // The counted reference stops the page being recycled AGAIN; it says
        // nothing about what happened before this CPU last looked. Only a
        // per-access call answers that, which is what the SafePtr is.
        [[nodiscard]] Ref mapping() const { return mappingPtr; }
        [[nodiscard]] uint64_t va() const { return searchVA; }
        [[nodiscard]] uint64_t lo() const { return rangeLo; }
        [[nodiscard]] uint64_t hi() const { return rangeHi; }   // inclusive
        explicit operator bool() const { return mappingPtr != nullptr; }

        // ─── The validity token (§3.1, DEC-051) ────────────────────────────
        //
        // The token IS `(searchVA, mappingPtr, rangeLo, rangeHi)` — the VA
        // together with the DECODED answer it produced. It is not a separate
        // object here, and that is deliberate: the identity half is a pointer
        // compared for equality, so it is only meaningful while the counted
        // reference that keeps that record alive is still held. Detaching the
        // token from the reference makes it possible to compare a recycled slab
        // slot equal to the record it replaced, which is the same class of bug
        // as the node-pointer token §3.1 rejects. Keeping them in one move-only
        // object makes the safe usage the only expressible one.
        //
        // What must NOT be inferred from that packaging is that the two have the
        // same meaning. §3.1: "The two have different lifetimes and conflating
        // them is a memory-safety defect." The reference guarantees the RECORD
        // stays alive. It guarantees nothing about the ADDRESS staying mapped —
        // a concurrent partial `munmap` shrinks a leaf's covered range with a
        // single slot write that retires nothing and marks nothing, so no other
        // signal exists for a blocked thread to observe. That is what
        // `CoreTree::revalidate` is for.

    private:
        void reset() {
            if (mappingPtr) {
                releaseMappingRef(mappingPtr);
                mappingPtr = Ref{};
            }
        }
        Ref      mappingPtr{};
        uint64_t searchVA = 0;
        uint64_t rangeLo = 0;
        uint64_t rangeHi = 0;
    };

    // ─── The seam handle (§5.5, §7.5; DEC-016 / DEC-078) ───────────────────
    //
    // "The one genuine leak across the seam is the descent cache, which holds a
    // reference to an interior core node and must learn on hit whether it is
    // dying."
    //
    // The handle is **opaque to the policy layer** — it never dereferences it —
    // and it is **counted**, which is half of §7.5's duality:
    //
    //   - the counted reference stops the node's memory being freed and reused
    //     as a *different* node. Without it a cached pointer could silently come
    //     to name a valid, unmarked, unrelated node, and the cache would then
    //     return a `Mapping` for the wrong address without crashing (ABA).
    //   - the dying mark, read on every hit, is what tells the entry its node
    //     has been detached. **The mark, not the reference, is what makes a
    //     cached node's CHILDREN safe to read**: a reference keeps the node
    //     itself alive and says nothing about the slots inside it.
    //
    // Three values, not one, because DEC-012 keeps level and base OUT of nodes:
    // descent derives level by counting and a subtree's base by masking the
    // search key, so a bare node pointer cannot be resumed from. Both are
    // intrinsic and immutable for the node's life — growth builds a parent
    // ABOVE the old root rather than relabelling it — so carrying them in the
    // handle is not a staleness hazard, unlike the `{root, level, base}` triple
    // decoded from a bucket word (§7.3).
    //
    // Move-only and self-releasing, for LookupResult's reason: a copy that did
    // not bump the count is a double release, and a dropped handle is a node
    // pinned for the machine's life. `release()` is still the seam's third
    // operation and the cache calls it explicitly — the destructor is the
    // backstop, not the intended path.
    //
    // **The release may destroy.** Invariant 24 makes the destructor release
    // nothing, so that is bounded straight-line work and legal even inside a
    // read section in `#PF` context: a refcount-zero destroy is the DESTRUCTOR,
    // not a deleter, and §7.6's deleters-run-outside-any-section rule does not
    // reach it (§7.5).
    template <GeometryDescriptor G>
    class PinnedNode {
    public:
        PinnedNode() = default;
        PinnedNode(const PinnedNode&)            = delete;
        PinnedNode& operator=(const PinnedNode&) = delete;

        PinnedNode(PinnedNode&& o) noexcept
            : node(o.node), nodeBase(o.nodeBase), nodeLevel(o.nodeLevel) {
            o.node = NodeRef();
        }
        PinnedNode& operator=(PinnedNode&& o) noexcept {
            if (this != &o) {
                release();
                node = o.node; nodeBase = o.nodeBase; nodeLevel = o.nodeLevel;
                o.node = NodeRef();
            }
            return *this;
        }
        ~PinnedNode() { (void)release(); }

        explicit operator bool() const { return static_cast<bool>(node); }

        // The seam's third operation. Idempotent, so the destructor backstop
        // cannot double-release what the cache already released.
        //
        // Returns whether this release was the last one and therefore destroyed.
        // That is instrumentation, not a lifetime disclosure: DEC-016's cost
        // claim is that "atomics fall on cache turnover rather than on faults",
        // and a policy layer that cannot count its own destroys cannot check it.
        bool release() {
            if (!node) return false;
            // ACQ_REL rather than DEC-069's release-plus-acquire-fence, for the
            // reason kRefcountReleaseAcquire states: the fence form is correct
            // and invisible to the release gate (D-029).
            const uint64_t prior = node.refcount().fetch_sub(1, kRefcountReleaseAcquire);
            assert(prior != 0, "radix: a descent-cache pin released below zero");
            const bool destroyed = (prior == 1);
            if (destroyed) NodeOps<G>::destroy(nodeLevel, node);
            node = NodeRef();
            return destroyed;
        }

    private:
        template <GeometryDescriptor, typename, unsigned> friend class CoreTree;

        PinnedNode(NodeRef n, unsigned level, uint64_t base)
            : node(n), nodeBase(base), nodeLevel(level) {}

        NodeRef  node{};
        uint64_t nodeBase  = 0;
        unsigned nodeLevel = 0;
    };

#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
    // ─── Mode 5: inside a descent-cache HIT ────────────────────────────────
    //
    // Mode 4 split a lookup into `lookupInner` versus DEC-060's pump and left
    // `lookupInner` at a MINIMUM of ~334 instructions over a run that hit the
    // descent cache 83% of the time. A hit is supposed to be "index four
    // entries, compare a range, resume" — so 334 says most of a hit is
    // somewhere inside `resumeDescent`, and mode 4 cannot say where.
    //
    // This brackets the three parts of a resume separately:
    //
    //   range   — the seam's own out-of-range guard, which is pure geometry
    //             arithmetic: `nodeSpan(G, pin.nodeLevel)` at a RUNTIME level.
    //   guard   — the RCU section entry (interrupt-masked, RCU-DEC-024) plus
    //             the acquire mark load through `SafePtr`.
    //   descend — `descendFromLocked`: one level on a hit, so this is the price
    //             of a single slot load, one codec decode, one `acquireRef`
    //             and the `LookupResult`.
    //
    // Lives here rather than in DescentCache.h because two of the three
    // brackets are inside this header's `resumeDescent`. The brackets NEST
    // inside mode 4's `inner` bracket, which is why this is a separate mode
    // rather than more fields on that one — probe pairs inside a bracket
    // inflate it, and mode 4's total is the figure that must stay honest.
    inline uint64_t probe5Counter() noexcept {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // MINIMA, for the P4-ITEM-002 reason mode 4 records: interrupts are live in
    // the stress loop, so a timer inside a bracket inflates that sample by the
    // whole handler and the contamination dilutes rather than averaging out.
    inline Atomic<uint64_t> gProbe5BaseMin{0};
    inline Atomic<uint64_t> gProbe5RangeMin{0};
    inline Atomic<uint64_t> gProbe5GuardMin{0};
    inline Atomic<uint64_t> gProbe5DescendMin{0};
    inline Atomic<uint64_t> gProbe5ResumeMin{0};
    inline Atomic<uint64_t> gProbe5Count{0};

    inline void probe5RecordMin(Atomic<uint64_t>& slot, uint64_t sample) noexcept {
        uint64_t cur = slot.load(kCacheAccounting);
        while ((cur == 0 || sample < cur) &&
               !slot.compare_exchange_weak(cur, sample, kCacheAccounting,
                                          kCacheAccounting)) {}
    }
#endif

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
    // These were briefly spelled `{}` rather than `{0}`. `Atomic<T>::Atomic(T)`
    // used to perform a SEQ_CST store, and a `TreeStats` is not a singleton — a
    // tree is "a VALUE, deliberately" (ClusterTable::treeFor), so every lookup
    // constructed one and these initializers were nine locked read-modify-writes
    // on a stack object per FAULT. That was a workaround at the call site for a
    // defect in the type, and the type has since been fixed: `Atomic(T)` is now
    // `constexpr` and initializes non-atomically, because construction is not
    // observable by another thread. So `{0}` is free again, and says what it
    // means.
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
        // Slots commit declined to act on because the attempt holds no claim bit
        // for them — a clearing row that appeared in the window between
        // re-dispatch and commit (D-013). A legal execution, so a counter rather
        // than an assert, but a number that climbs into the operation count means
        // the read pass is under-claiming rather than losing a genuine race.
        Atomic<uint64_t> unheldRowsSkipped{0};
        // §6.5 decompositions entered (including recursive ones). A `MAP_FIXED`
        // shape that decomposes when the geometry says it should not is a
        // budget-tuning signal, not a correctness one.
        Atomic<uint64_t> decompositions{0};
        // §7.1: attempts abandoned because this CPU's DeferredRelease pool was
        // momentarily short — the records exist and are in flight. Each one costs
        // a grace period, so this is the number that says whether the per-CPU
        // POPULATION is big enough for the workload.
        Atomic<uint64_t> recordShortfalls{0};
        // D-059: attempts refused because their record demand exceeded the budget,
        // and therefore decomposed. Counted apart from `decompositions` and from
        // `recordShortfalls` because it is a third phenomenon and the three want
        // opposite responses: a shortfall says grow the population, an overrun says
        // the workload legitimately displaces more distinct mappings in one attempt
        // than a pool holds, and a plain decomposition is about detachment size.
        // Confusing the first two is what ITEM-084 did.
        Atomic<uint64_t> recordBudgetOverruns{0};

        void reset() {
            attempts.store(0, kPrivateInit);
            maxRetries.store(0, kPrivateInit);
            completions.store(0, kPrivateInit);
            claimConflicts.store(0, kPrivateInit);
            redispatchChanges.store(0, kPrivateInit);
            unheldRowsSkipped.store(0, kPrivateInit);
            decompositions.store(0, kPrivateInit);
            recordShortfalls.store(0, kPrivateInit);
            recordBudgetOverruns.store(0, kPrivateInit);
        }
    };

    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    class CoreTree {
    public:
        using Ops = NodeOps<G>;
        // Built over the SAME window base as the slot codec — see
        // SlotCodec::BasePolicy for why that is re-exported rather than
        // re-specified.
        using BucketCodecT = BucketCodec<G, typename Codec::BasePolicy>;

        [[nodiscard]] TreeStats& stats() { return counters; }
        [[nodiscard]] const TreeStats& stats() const { return counters; }

        // A tree rooted at `rootLevel` covering `[base, base + nodeSpan(rootLevel))`.
        // The base is span-aligned by construction: a cluster root is a node of
        // the conceptual full-depth tree, so prefix indexing inside it is free
        // and growth is exact (the old root becomes precisely one child slot of
        // its new parent, so no mapping moves).

        // The tree's domain is per-address-space (DEC-072 / RCU-DEC-043). One
        // global domain was rejected on the merits: teardown's no-new-users
        // precondition becomes unsatisfiable at process exit, the per-address-
        // space residue bounds become unreachable, and ONE stalled fault-path
        // reader in any process stalls reclamation for every process.
        // `pools` is the per-address-space DeferredRelease population (§7.1,
        // DEC-068). Required, not optional: a tree without it would have to fall
        // back on the synchronous release, which is §10's named use-after-free
        // and passes every single-threaded test.
        [[nodiscard]] bool init(unsigned rootLevel, uint64_t base, kernel::rcu::Domain& d,
                                DeferredReleasePools& pools) {
            assert(d.initialized(), "radix: the tree's RCU domain must be initialised first");
            // The pool's population IS the per-attempt record budget (D-059), so
            // there is no per-OPERATION ceiling to satisfy any more — that absence is
            // the point. But "any size >= 1" was wrong, which this used to claim
            // (D-070): a probed `OnlyIfEmpty` placement draws records too, and an
            // overrun there returns `NeedsDecomposition`, which `applyOrDecompose`
            // forbids for `OnlyIfEmpty` — debug panics, release half-places. So the
            // floor is geometry-derived, and asserted here because this is where the
            // geometry is known.
            assert(pools.recordBudget() >= 1,
                   "radix: the DeferredRelease pool holds no records, so no displacing "
                   "operation can ever complete (D-059)");
            domain = &d;
            releasePools = &pools;
            assert(rootLevel >= 1 && rootLevel <= G.levelCount,
                   "radix: root level out of range");
            assert(base % Geo<G>::nodeSpan(rootLevel) == 0,
                   "radix: cluster base must be span-aligned — prefix indexing inside the "
                   "cluster depends on it");
            void* p = Ops::alloc(rootLevel, /*initialRefcount=*/1);
            if (!p) return false;
            rootRef = NodeRef(p);
            level0  = rootLevel;
            baseVA  = base;
            return true;
        }

        // The cluster form: the root lives in a bucket word, so there is nothing
        // to allocate here and nothing to fail. `ClusterTable::createCluster`
        // has already published a root (or the caller has adopted one); this
        // only says which word to read.
        void bindToBucket(BucketTable& t, size_t index, kernel::rcu::Domain& d,
                          DeferredReleasePools& pools) {
            assert(d.initialized(), "radix: the tree's RCU domain must be initialised first");
            assert(pools.recordBudget() >= 1,
                   "radix: the DeferredRelease pool holds no records (D-059) — see init");
            assert(index < kBucketCount, "radix: bucket index out of range");
            domain       = &d;
            releasePools = &pools;
            bucketTable  = &t;
            bucketIndex  = index;
            rootRef      = NodeRef();
            level0       = 0;
            baseVA       = 0;
        }

        // Phase 1 teardown. Phase 3 replaces this with §7.4's unit-decomposed
        // walk; the structural obligation it already honours is that every node
        // releases the Mapping references its own leaf slots hold, and that
        // nothing recurses through a destructor.
        void destroyTree() {
            const RootBinding b = quiescedBinding();
            if (!b) return;
            releaseSubtree(b.root, b.level);
            if (bucketTable != nullptr) {
                buckets()->entries[bucketIndex].store(0, kPrivateInit);
            } else {
                rootRef = NodeRef();
            }
        }

        // ─── §7.4's unit-decomposed teardown walk (DEC-100) ────────────────
        //
        // The walk `destroyAddressSpace` runs, and the difference from
        // `destroyTree` above is not tidiness. Each node is torn down as its own
        // UNIT — one read section, claim, mark, unlink, **retire** — and three
        // properties fall out that the synchronous form does not have:
        //
        //   - **Every node is MARKED before it is unlinked.** §7.4: "Without any
        //     marking at all, teardown would be the only unlink path setting no
        //     mark, leaving a foreign CPU's surviving cache entry pointed at a
        //     node that is unlinked, unmarked, alive, and holding slots that
        //     point at freed children." That entry is Phase 4's descent cache,
        //     which is why the marking is a PREREQUISITE for it rather than a
        //     nicety here.
        //   - **Every release is deferred**, so a `DeferredRelease` record still
        //     in a bag and a node deleter releasing the same `Mapping` both land
        //     inside the one drain that follows, and the count reaches zero
        //     exactly once. The synchronous walk had to be preceded by an extra
        //     drain to avoid releasing through a record onto an already-destroyed
        //     record (D-032).
        //   - **The parent-slot bit is released at unit end**, while the node's
        //     own whole-node mask never is. §7.4 is explicit: held to the
        //     mark-and-retire, the parent-slot bit "would sit in the later unit's
        //     `fetch_or` prior and fire the quiescence assert above on every
        //     process exit" — assert erosion, from a mechanical mistake.
        //
        // The walk carries a node pointer across its children's section closes,
        // which §7.3 forbids in general. It is sound HERE for a reason §7.4
        // states rather than assumes: the walk runs after the dying flag,
        // thread destruction and `synchronize`, so it is not merely uncontended
        // but **unobserved** — there is no actor that could reclaim what we
        // hold. Nothing else in the tree gets to make this argument.
        void tearDownUnits() {
            const RootBinding b = quiescedBinding();
            if (!b) return;
            tearDownUnitAt(b.root, b.level, NodeRef(), 0, /*isRoot=*/true);
        }

    private:
        void tearDownUnitAt(NodeRef node, unsigned level,
                            NodeRef parent, unsigned parentSlot, bool isRoot) {
            // Children first: a node's unit unlinks it from ITS parent, so the
            // parent must still be linked when the child's unit runs.
            const unsigned n = Geo<G>::valence(level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    tearDownUnitAt(NodeRef(Codec::decodeChild(w)), level + 1, node, i, false);
                }
            }

            Attempt a;
            a.binding = RootBinding{node, level, 0};
            kernel::rcu::ReadGuard guard(*domain);

            // The whole-node claim. DEC-065's quiescence assert on the operand:
            // by here the walk is unobserved, so ANY claim bit or mark already
            // set means somebody else is still working — which is the failure
            // this sequence exists to make impossible, and the assert is how a
            // future reordering of it announces itself.
            const uint64_t mask  = valenceMask(G, level);
            const uint64_t prior = node.stateWord().fetch_or(mask, kClaimAcquire);
            assert((prior & (mask | state::kMarkMask)) == 0,
                   "radix teardown: a node was already claimed or marked when its unit ran — "
                   "the walk is supposed to be unobserved by then (§7.4/DEC-065)");
            (void)prior;
            const bool added = a.claims.addOrMerge(node, level, 0, mask, true);
            assert(added, "radix teardown: claim set capacity");
            (void)added;
            a.claims.entries[a.claims.count - 1].issued = true;
            a.claims.entries[a.claims.count - 1].held   = true;

            uint64_t parentMask = 0;
            if (!isRoot) {
                // The parent-slot bit, taken so the unlink store below is an
                // ordinary claimed publish rather than an unprotected write.
                parentMask = uint64_t{1} << parentSlot;
                const uint64_t pprior =
                    parent.stateWord().fetch_or(parentMask, kClaimAcquire);
                assert((pprior & parentMask) == 0,
                       "radix teardown: the parent slot was already claimed");
                (void)pprior;
                const bool padded =
                    a.claims.addOrMerge(parent, level - 1, 0, parentMask, false);
                assert(padded, "radix teardown: claim set capacity (parent)");
                (void)padded;
                a.claims.entries[a.claims.count - 1].issued = true;
                a.claims.entries[a.claims.count - 1].held   = true;
            }

            a.phase = Attempt::Phase::Committing;
            markUnderClaim(node, level, a);

            if (isRoot) {
                // DEC-100's final unit: the bucket word is cleared inside this
                // unit's section, and the root is retired like any other node —
                // since DEC-103 there is no descriptor object, so the bucket
                // word's reference is released by the root's OWN deleter. One
                // object, one releaser.
                if (bucketTable != nullptr) {
                    buckets()->entries[bucketIndex].store(0, kBucketPublishSuccess);
                } else {
                    rootRef = NodeRef();
                }
            } else {
                // §11's named exemption: a post-mark parent-slot store is not an
                // acquisition after a mark.
                publishAfterMark(parent, parentSlot, 0, a);
                // Released at unit END — see the header comment. The node's own
                // whole-node mask is deliberately NOT released.
                releaseClaim(parent.stateWord(), parentMask);
                if (auto* e = a.claims.find(parent)) e->held = false;
            }

            Ops::template retire<Codec>(*domain, level, node);
            // The node's own claim is never released: it is marked and retired,
            // and §7.4 scopes the never-release rule to exactly that.
            if (auto* e = a.claims.find(node)) e->held = false;
        }

    public:

        // The binding, read WITHOUT a section. Legal only on a quiesced tree —
        // between operations, with no concurrent mutator — which is exactly the
        // condition every caller below already requires for its own reasons
        // (teardown, the validators, the node census). Named separately from
        // `currentBinding` so the two cannot be confused: using this one inside
        // an operation would drop the protected-link-load primitive.
        [[nodiscard]] RootBinding quiescedBinding() const {
            if (bucketTable == nullptr) return RootBinding{rootRef, level0, baseVA};
            const uint64_t w = buckets()->entries[bucketIndex].load(kQuiescedRead);
            const auto d = BucketCodecT::decode(w, bucketIndex);
            return RootBinding{NodeRef(d.root), d.level, d.base};
        }

        // Quiesced introspection, all of it. On a bucket-homed tree these read
        // the bucket word without a section, so they answer for the cluster as
        // it stands between operations and must not be used inside one.
        [[nodiscard]] bool valid() const { return static_cast<bool>(quiescedBinding()); }
        [[nodiscard]] unsigned rootLevel() const { return quiescedBinding().level; }
        [[nodiscard]] uint64_t base() const { return quiescedBinding().base; }
        [[nodiscard]] uint64_t span() const { return Geo<G>::nodeSpan(quiescedBinding().level); }
        [[nodiscard]] NodeRef root() const { return quiescedBinding().root; }
        [[nodiscard]] bool contains(uint64_t va) const {
            const RootBinding b = quiescedBinding();
            if (!b) return false;
            return va >= b.base && va < b.base + Geo<G>::nodeSpan(b.level);
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
                // §5.1: "A lookup range-checks the VA against the cluster's
                // current span, which is what answers 'unmapped'." Inside the
                // section and against the binding the descent will use — a check
                // against a span read outside it would be answering about a
                // different cluster than the one traversed.
                r = descendLocked(currentBinding(), va);
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

        // ─── Revalidation (§3.1, DEC-051 / DEC-070) ────────────────────────
        //
        // The caller that blocked on a pager round-trip re-opens a section,
        // re-descends to its VA, and compares the freshly-decoded
        // `(Mapping identity, absolute range)` against what it was told before.
        // Three things about this are load-bearing and each has a wrong version
        // that looks right:
        //
        //   - **Re-descend, do not remember.** An earlier token shape was
        //     `(node, slot index, slot word)`, which holds a LINK-LOADED node
        //     pointer across a section close — forbidden by §7.3, and unsafe for
        //     exactly the reason that rule exists: the counted reference is on
        //     the `Mapping`, not on the node, so between the close and the
        //     pager's reply the node can be reclaimed, graced, destroyed and its
        //     slab slot recycled. Revalidation would then read a live,
        //     addressable, unrelated object and could compare EQUAL.
        //
        //   - **Compare the decoded values, never the raw word.** A slot word's
        //     range field is level-relative — 8 KiB units at C0, 4 KiB at C1 —
        //     so a C0 leaf with range [0,0] covers 8 KiB and the C1 leaf that
        //     survives a 4 KiB `munmap`'s shortfall subdivision has the same
        //     `Mapping` bits, the same tag and the same [0,0] field. A
        //     bit-identical word for a halved range: comparing words compares
        //     EQUAL on an address that was just unmapped.
        //
        //   - **A `true` here is not a guarantee, and the caller must not treat
        //     it as one** (DEC-070). It proves the coverage held at a point
        //     inside this section and nothing more. Writers never wait on
        //     readers, so a shrink or an `mprotect` split can commit BETWEEN
        //     this compare and the externally-visible commitment the caller
        //     makes on its strength. The VMM must re-check under the
        //     interlock it shares with its PTE-removal path — the per-page-table
        //     lock. **The token detects every decision that committed before the
        //     compare; the install-side interlock is what excludes the ones that
        //     commit after it.** Without that half, a thread that revalidated
        //     writable `v` can install a writable PTE after a concurrent
        //     `mprotect(v, PROT_READ)` has returned — W^X defeated by an
        //     ordinary race.
        //
        // Takes the `LookupResult` rather than a detached token so the counted
        // reference is structurally still held: the identity half is a pointer
        // compared for equality, and a released record's slab slot can be
        // recycled into a new `Mapping` at the same address.
        [[nodiscard]] bool revalidate(const LookupResult& held) const {
            if (!held) return false;
            LookupResult now;
            {
                kernel::rcu::ReadGuard guard(*domain);
                now = descendLocked(currentBinding(), held.va());
            }
            // Note the fresh result's own counted reference is taken and dropped
            // by this call. That is not waste — it is §7.3's acquisition law
            // applied without an exception: the reference must be taken inside
            // the section that observed the link, and a "peek without acquiring"
            // variant would be a second, weaker path through the one rule the
            // reader side has.
            return static_cast<bool>(now) &&
                   now.mapping() == held.mapping() &&
                   now.lo() == held.lo() &&
                   now.hi() == held.hi();
        }

        // ─── The pin site (DEC-078) ────────────────────────────────────────
        //
        // Where a descent would take a descent-cache pin: the node it was
        // standing on at the caller's chosen level, together with the level and
        // base that DEC-012 keeps out of nodes and that a resume therefore
        // cannot recover from the pointer alone.
        //
        // A site is NOT a reference. It is filled in by a descent and is only
        // meaningful inside that descent's section; `pinLocked` is what turns
        // one into a counted handle that outlives it, and it is the only thing
        // in the tree allowed to do so.
        struct PinSite {
            NodeRef  node{};
            unsigned level = 0;
            uint64_t base  = 0;
            bool     valid = false;
        };

        // "at a caller-chosen level" (DEC-078). This asks for the DEEPEST node
        // the descent reaches — DEC-016's "interior node directly above the last
        // accessed mapping", which is what the cache wants by default. A numeric
        // level asks for the deepest node at or above it, so a descent that
        // terminates shallower than the request still yields a site rather than
        // nothing.
        static constexpr unsigned kPinAtDeepest = ~0u;

        [[nodiscard]] LookupResult descendLocked(const RootBinding& bind, uint64_t va) const {
            return descendLocked(bind, va, kPinAtDeepest, nullptr);
        }

        [[nodiscard]] LookupResult descendLocked(const RootBinding& bind, uint64_t va,
                                                 unsigned pinLevel, PinSite* outSite) const {
            if (!bind) return {};
            if (va < bind.base || va >= bind.base + Geo<G>::nodeSpan(bind.level)) return {};
            return descendFromLocked(bind.root, bind.level, bind.base, va, pinLevel, outSite);
        }

        // The descent proper, resumable from any node. `resumeDescent` enters
        // here with a pinned node rather than with the root binding, and that is
        // the entire mechanical content of the cache: a hit is this loop started
        // partway down.
        [[nodiscard]] LookupResult descendFromLocked(NodeRef node, unsigned level,
                                                     uint64_t nodeBase, uint64_t va,
                                                     unsigned pinLevel,
                                                     PinSite* outSite) const {
            for (;;) {
                // Recorded BEFORE the slot load, so a descent that terminates
                // here still leaves the node it terminated on as the site.
                if (outSite != nullptr && level <= pinLevel) {
                    *outSite = PinSite{node, level, nodeBase, true};
                }
                const unsigned idx = slotIndexFor(level, nodeBase, va);
                // Invariant 21: EVERY link load on a reader path goes through
                // the protected-link-load primitive — protectWord supplies the
                // framework's named ACQUIRE and the debug section-open assert,
                // and the codec supplies the decode. That composite is this
                // consumer's RcuPtr (DEC-073); there is no exception.
                const uint64_t word = kernel::rcu::protectWord(*domain, node.slot(idx));
                const uint64_t slotBase = nodeBase + uint64_t{idx} * Geo<G>::slotSpan(level);

                switch (Codec::kindOf(word)) {
                case SlotKind::Empty:
                    return {};
                case SlotKind::Leaf: {
                    // The `covers` hook resolves the NEGATIVE answer without
                    // dereferencing the Mapping at all, which is the common
                    // unmapped-VA lookup (DEC-022).
                    if (!Codec::covers(word, va - slotBase, level)) return {};
                    // DEC-073: the slot word came through `protectWord`, and
                    // "whatever pointer it derives carries the SafePtr freshness
                    // obligation exactly as with `protect`". The obligation
                    // travels WITH the reference from here — `LookupResult`
                    // holds a `SafePtr`, so the caller's later reads are covered
                    // even if it resumes on another CPU after a pager
                    // round-trip, which a call made only here would not be.
                    const VMSubstrate::SafePtr<Mapping> m(
                        static_cast<Mapping*>(Codec::decodeLeaf(word)));
                    // §7.3's acquisition law: a counted reference is acquired
                    // ONLY inside the read section in which that link was
                    // observed. Taking it after the close would let the node be
                    // unlinked, retired, graced, released to zero and destroyed,
                    // and its slab slot handed to an unrelated allocation, before
                    // the fetch_add landed on a stranger.
                    acquireMappingRef(m);
                    uint64_t lo = 0, hi = 0;
                    Codec::absoluteRange(word, slotBase, level, lo, hi);
                    return LookupResult{m, va, lo, hi};
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

        // ─── The seam's three operations (DEC-078) ─────────────────────────
        //
        // pin — resume — release. Nothing else crosses, and the policy layer
        // never dereferences the handle.

        // **pin**: turn a site the caller's own descent produced into a counted
        // handle. §7.3's law is the whole contract — "a counted reference to a
        // node is acquired ONLY inside the read section in which that node's
        // link was observed" — and §11 asks for it to be asserted rather than
        // documented, so this is a `Locked` entry point taking the section from
        // its caller and checking that it exists.
        //
        // The check is one-sided by construction: the framework can tell us a
        // section is open on this domain, but not that it is the SAME section
        // the link was loaded in. What closes that half is the shape of the
        // call — a site is produced by `descendLocked` and consumed here, both
        // under one `ReadGuard` — plus §11's own test case, which fills a
        // candidate, closes the section, reclaims the node and requires the
        // later install to have re-descended rather than remembered.
        //
        // No count can be zero here: the node was reached by a link load in this
        // section, so any unlink of it is followed by a grace period this
        // section has not yet left, and its deleter's self-release cannot have
        // run.
        [[nodiscard]] PinnedNode<G> pinLocked(const PinSite& site) const {
            assert(site.valid, "radix: a pin of no node");
            kernel::rcu::assertInReadSection(*domain);
            site.node.refcount().fetch_add(1, kRefcountAcquire);
            return PinnedNode<G>(site.node, site.level, site.base);
        }

        // §7.5/DEC-078's answer to a hit. `Detached` doubles as "this address
        // space is gone", because teardown marks every node (invariant 23).
        enum class ResumeStatus : uint8_t { Ok, Detached, OutOfRange };

        struct ResumeResult {
            ResumeStatus status = ResumeStatus::Detached;
            LookupResult result;
        };

        // **resume**: a fresh section, an acquire load of the mark, then the
        // descent — in that order, and the order is load-bearing.
        //
        // Invariant 29 binds the mark load and the slot loads into ONE section,
        // and §7.5 says why that is the memory-safety story rather than the
        // refcount: the reference keeps the NODE alive and says nothing about
        // the slots inside it. For a node that is still live and linked a hit
        // cannot read a freed child, because reclamation clears the parent slot
        // before retiring the child and this descent re-loads that slot. The
        // freed-children hazard is real only for a node that is itself
        // unreachable — detachment and teardown — and both mark. Split the mark
        // load into its own section and the node can be detached between the two
        // sections, with the mark read clean and the children read freed.
        //
        // kCacheFreshnessLoad is the acquire counterpart to kMarkStore's
        // release. A release with no acquire opposite establishes nothing.
        [[nodiscard]] ResumeResult resumeDescent(const PinnedNode<G>& pin, uint64_t key) const {
            assert(static_cast<bool>(pin), "radix: a resume from an empty pin");
            ResumeResult out;

#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t pb0 = probe5Counter();
            const uint64_t pb1 = probe5Counter();
            probe5RecordMin(gProbe5BaseMin, pb1 - pb0);
            const uint64_t r0 = probe5Counter();
#endif
            // The entry's own tag check, re-done at the seam so the seam is
            // total rather than trusting. Free — no atomic, no section — and
            // deliberately NOT part of §7.5's generation-then-mark-then-descend
            // sequence: it answers "is this handle even about this key", which
            // the caller has already answered from the entry's VA range.
            const uint64_t span = Geo<G>::nodeSpan(pin.nodeLevel);
            const bool outOfRange = (key < pin.nodeBase || key >= pin.nodeBase + span);
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t r1 = probe5Counter();
            probe5RecordMin(gProbe5RangeMin, r1 - r0);
#endif
            if (outOfRange) {
                out.status = ResumeStatus::OutOfRange;
                return out;
            }

#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t g0 = probe5Counter();
#endif
            kernel::rcu::ReadGuard guard(*domain);
            const bool marked = state::isMarked(pin.node.stateWord().load(kCacheFreshnessLoad));
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t g1 = probe5Counter();
            probe5RecordMin(gProbe5GuardMin, g1 - g0);
#endif
            if (marked) {
                out.status = ResumeStatus::Detached;
                return out;
            }
            out.status = ResumeStatus::Ok;
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t d0 = probe5Counter();
#endif
            out.result = descendFromLocked(pin.node, pin.nodeLevel, pin.nodeBase, key,
                                           kPinAtDeepest, nullptr);
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
            const uint64_t d1 = probe5Counter();
            probe5RecordMin(gProbe5DescendMin, d1 - d0);
            gProbe5Count.fetch_add(1, kCacheAccounting);
#endif
            return out;
        }

        // ─── Chunked scanning (§5.5 DEC-083, §5.6 DEC-095) ─────────────────
        //
        // Enumeration and placement's free-run scan are the same walk with
        // different consumers, and §5.5 says so — the enumeration is "chunked
        // exactly like DEC-095's scan". One implementation, so the two cannot
        // drift into disagreeing about what "the next thing after this VA" is.
        //
        // `scanStepLocked` answers, for one VA: is it mapped, and how far does
        // the answer hold? An occupied step reports the leaf's absolute range so
        // the caller can emit it and step past; an empty step reports how much
        // emptiness it can prove, so the caller can skip a whole C0 slot's
        // 32 MiB in one move rather than crawling the floor.
        //
        // The empty span is DELIBERATELY conservative — it is what this descent
        // can prove, not the maximal run. A caller uses it to advance a cursor,
        // and under-reporting costs an extra step while over-reporting would
        // skip a live mapping.
        struct ScanStep {
            bool occupied = false;
            // A `SafePtr`, not a `Mapping*`, and for the reason D-044 settled for
            // `LookupResult`: this pointer LEAVES the tree, and a consumer that
            // dereferences it — every real one does, to read a base VA or a
            // protection — may be making its CPU's first touch of that page.
            // The obligation therefore travels with the pointer rather than
            // being an unwritten precondition on the callback (D-049).
            VMSubstrate::SafePtr<Mapping> mapping{};   // occupied only
            uint64_t lo = 0;
            uint64_t hi = 0;                // inclusive; the range, or the hole
        };

        [[nodiscard]] ScanStep scanStepLocked(const RootBinding& bind, uint64_t va) const {
            ScanStep out;
            const uint64_t clusterEnd = bind.base + Geo<G>::nodeSpan(bind.level) - 1;
            if (!bind || va < bind.base || va > clusterEnd) {
                out.lo = va;
                out.hi = va;
                return out;
            }

            NodeRef  node     = bind.root;
            unsigned level    = bind.level;
            uint64_t nodeBase = bind.base;

            for (;;) {
                const unsigned idx      = slotIndexFor(level, nodeBase, va);
                const uint64_t span     = Geo<G>::slotSpan(level);
                const uint64_t slotBase = nodeBase + uint64_t{idx} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t word     = kernel::rcu::protectWord(*domain, node.slot(idx));

                switch (Codec::kindOf(word)) {
                case SlotKind::Empty:
                    out.lo = slotBase;
                    out.hi = slotEnd;
                    return out;

                case SlotKind::Leaf: {
                    uint64_t lo = 0, hi = 0;
                    Codec::absoluteRange(word, slotBase, level, lo, hi);
                    if (va >= lo && va <= hi) {
                        out.occupied = true;
                        out.mapping  = static_cast<Mapping*>(Codec::decodeLeaf(word));
                        out.lo = lo;
                        out.hi = hi;
                        return out;
                    }
                    // A leaf that does not cover `va`: the hole runs to the
                    // leaf's start if we are below it, or to the slot end if we
                    // are above it. Both are exact, because a leaf's sub-range
                    // is the only occupied part of its slot.
                    out.lo = va;
                    out.hi = (va < lo) ? lo - 1 : slotEnd;
                    return out;
                }

                case SlotKind::Child:
                    node     = NodeRef(Codec::decodeChild(word));
                    nodeBase = slotBase;
                    level++;
                    break;
                }
            }
        }

        // A resumable position in a scan. **A VA and nothing else** — §7.3
        // forbids uncounted pointers crossing a section close, not addresses,
        // and this is the whole reason the enumeration can be chunked at all.
        // Carrying a node pointer here would be the token mistake of §3.1 in a
        // second place.
        struct ScanCursor {
            uint64_t next = 0;
            uint64_t end  = 0;         // inclusive
            bool     done = true;

            [[nodiscard]] bool finished() const { return done || next > end; }
        };

        [[nodiscard]] ScanCursor scanFrom(uint64_t lo, uint64_t hi) const {
            ScanCursor c;
            c.next = lo;
            c.end  = hi;
            c.done = (lo > hi);
            return c;
        }

        // One chunk: at most `ScanChunk` slots examined inside ONE read section,
        // then the section closes with only the cursor surviving.
        //
        // The bound is the point, not a tuning nicety. A whole-cluster descent
        // in one section is ~10^3 nodes of non-preemptible, domain-wide
        // EBR-stalling work — the exact cost DEC-077 bounds at 64 for
        // detachment, and DEC-095 does not get to reintroduce it at 16x.
        //
        // `fn(SafePtr<Mapping>, lo, hi)` is called for each occupied range in
        // ascending VA order. A `SafePtr` because the callback's reads are
        // cross-CPU first touches of vmsmalloc memory (D-049) — the same reason
        // `LookupResult` holds one. Ranges are emitted PER LEAF: a mapping named by four slots
        // produces four adjacent pairs naming the same record, because coalescing
        // across a chunk boundary would need state that outlives the section.
        // Consumers that want merged spans coalesce on the way out.
        template <typename F>
        size_t enumerateChunk(ScanCursor& c, F&& fn) const {
            if (c.finished()) { c.done = true; return 0; }
            size_t emitted = 0;
            kernel::rcu::ReadGuard guard(*domain);
            const RootBinding bind = currentBinding();
            for (unsigned budget = 0; budget < kScanChunk; budget++) {
                if (c.next > c.end) { c.done = true; break; }
                const ScanStep step = scanStepLocked(bind, c.next);
                if (step.occupied) {
                    const uint64_t lo = step.lo > c.next ? step.lo : c.next;
                    const uint64_t hi = step.hi < c.end  ? step.hi : c.end;
                    fn(step.mapping, lo, hi);
                    emitted++;
                }
                // Advance past whatever this step proved, occupied or not. The
                // saturating add is what keeps a range ending at the top of the
                // address space from wrapping the cursor back to zero.
                if (step.hi >= UINT64_MAX - 1) { c.done = true; break; }
                c.next = step.hi + 1;
            }
            return emitted;
        }

        // The convenience loop. Sections open and close between chunks, which is
        // exactly what a caller must NOT paper over by holding one open around
        // this — the staleness contract below is the price of that.
        //
        // **Staleness, stated rather than implied** (the DEC-095 shape): a range
        // mapped or unmapped BEHIND the cursor by a concurrent operation is
        // invisible to this enumeration. That is an accepted snapshot, not a
        // bug: the alternative is one section over the whole cluster, which is
        // the cost being avoided.
        template <typename F>
        size_t enumerate(uint64_t lo, uint64_t hi, F&& fn) const {
            ScanCursor c = scanFrom(lo, hi);
            size_t total = 0;
            while (!c.finished()) total += enumerateChunk(c, fn);
            return total;
        }

        // ─── The chunked free-run scan (§5.6, DEC-095) ─────────────────────
        //
        // DEC-095 is a correction to the original O(1)-probe claim, and the
        // correction is the reason this exists at all: **the occupancy probing
        // runs against is per-CLUSTER, not per-address-space.** A process with a
        // few hundred MiB in a 1 GiB cluster probes at tens of percent
        // occupancy, not the sub-0.1% the O(1) figure was computed for. So the
        // scan is "a priced placement slow path, not a rare fallback".
        //
        // Chunked for the same reason the enumeration is, and it shares the same
        // step primitive. **The chunking makes the output a CANDIDATE, not a
        // reservation**: a section close discards everything but the VA cursor,
        // so DEC-007's fused verify-is-install holds only *within* the
        // installing attempt. The scan hands a candidate to a fresh
        // `OnlyIfEmpty` attempt whose own read pass re-verifies it, and a
        // candidate gone stale by then resumes the scan at the cursor.
        //
        // The cursor is monotonic over the cluster, so the loop terminates: at
        // an install, or at the cluster's end with no run found. That end state
        // is defined as **"no free run observed by THIS scan"** — a run freed
        // *behind* the cursor by a concurrent `munmap` is invisible to it. An
        // accepted false negative whose cost is an unnecessary growth or an
        // extra cluster; placement still succeeds and nothing is unmapped
        // wrongly.
        struct FreeRunCursor {
            uint64_t next     = 0;   // where the next chunk resumes
            uint64_t end      = 0;   // inclusive
            uint64_t runStart = 0;   // start of the empty run in progress
            bool     inRun    = false;
            bool     done     = true;

            [[nodiscard]] bool finished() const { return done; }
        };

        [[nodiscard]] FreeRunCursor freeRunScanFrom(uint64_t lo, uint64_t hi) const {
            FreeRunCursor c;
            c.next = lo;
            c.end  = hi;
            c.done = (lo > hi);
            return c;
        }

        // One chunk. Returns true having set `outVA` when a run of `bytes`
        // aligned to `align` has been observed; otherwise advances the cursor.
        //
        // Every field of the cursor is an ADDRESS or a flag — §7.3 forbids
        // uncounted pointers crossing a close, and `runStart` in particular is
        // the state that would be tempting to keep as a node.
        [[nodiscard]] bool findFreeRunChunk(FreeRunCursor& c, uint64_t bytes,
                                            uint64_t align, uint64_t& outVA) const {
            if (c.done) return false;
            kernel::rcu::ReadGuard guard(*domain);
            const RootBinding bind = currentBinding();
            if (!bind) { c.done = true; return false; }

            for (unsigned budget = 0; budget < kScanChunk; budget++) {
                if (c.next > c.end) { c.done = true; return false; }

                const ScanStep step = scanStepLocked(bind, c.next);
                if (step.occupied) {
                    c.inRun = false;
                } else {
                    if (!c.inRun) { c.inRun = true; c.runStart = c.next; }
                    // The run so far, measured from its first ALIGNED address:
                    // a run that starts mid-granule offers less than its length.
                    const uint64_t aligned = (c.runStart + align - 1) & ~(align - 1);
                    const uint64_t runEnd  = step.hi < c.end ? step.hi : c.end;
                    if (runEnd >= aligned && (runEnd - aligned + 1) >= bytes) {
                        outVA = aligned;
                        // Resume AFTER what we are handing out, so a stale
                        // candidate does not make the scan hand out the same VA
                        // forever — which is the shape that turns a lost race
                        // into a livelock.
                        c.next     = aligned + bytes;
                        c.inRun    = false;
                        if (c.next > c.end) c.done = true;
                        return true;
                    }
                }
                if (step.hi >= UINT64_MAX - 1) { c.done = true; return false; }
                c.next = step.hi + 1;
            }
            return false;
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
        [[nodiscard]] ApplyStatus apply(uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value,
                                        ApplyMode mode = ApplyMode::Overwrite) {
            assert(lo <= hi, "radix: empty operation range");
            if (bucketTable == nullptr) {
                assert(rootRef, "radix: apply on an uninitialised tree");
                assert(contains(lo) && contains(hi),
                       "radix: operation range escapes the cluster — DEC-058 decomposes per "
                       "cluster before the core tree is involved");
            } else {
                // DEC-080: "the mechanism never sees a range that crosses a
                // bucket boundary" — the VMM decomposes those into one mapping
                // per bucket before the tree is involved. Whether the range fits
                // the cluster's CURRENT span is a different question, and it is
                // asked per attempt against that attempt's own binding, because
                // a concurrent growth can only widen it.
                assert(bucketIndexFor<G>(lo) == bucketIndex &&
                       bucketIndexFor<G>(hi) == bucketIndex,
                       "radix: operation range crosses a bucket boundary (DEC-080)");
            }

            assert(mode == ApplyMode::Overwrite || value != nullptr,
                   "radix: OnlyIfEmpty with a null value is a clear that refuses to clear "
                   "anything — the caller means one of the two things, not neither");
            // ─── The OnlyIfEmpty record floor, asserted per MODE (D-070) ────
            //
            // A first attempt put this at bind time and it was wrong: it forbade the
            // small pools that Overwrite-only consumers — including this suite's
            // deliberately starved fixtures — are entitled to. The requirement is
            // conditional on the OPERATION, so it belongs where the mode is known.
            //
            // Probed placement draws records (its occupancy gate passes a
            // non-overlapping survivor, which dispatches `Subdivide`), and an overrun
            // returns `NeedsDecomposition` — which `applyOrDecompose` forbids for
            // `OnlyIfEmpty`, because no single unit can establish "the whole range was
            // empty". Debug panics there; release would decompose as Overwrite and
            // half-place the mapping. Two edge slots per level, one record each.
            assert(mode == ApplyMode::Overwrite ||
                       releasePools->recordBudget() >= placementRecordFloor(G),
                   "radix: an OnlyIfEmpty placement against a record budget below the "
                   "edge-subdivision floor (2 per level) — it can overrun and reach "
                   "applyOrDecompose's OnlyIfEmpty assert (D-070)");
            return applyOrDecompose(lo, hi, value, mode, 0);
        }

        // One unit: the attempt/retry loop of §6.1, which is what §6.5 means by
        // "each unit is an ordinary attempt with its own section, claim set and
        // marks". Returns `NeedsDecomposition` rather than decomposing, so the
        // caller decides whether this range is a whole operation or one slot of
        // somebody's decomposition.
        [[nodiscard]] ApplyStatus runToCompletion(uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value,
                                                  ApplyMode mode = ApplyMode::Overwrite) {
            unsigned retryCount = 0;
            unsigned consecutiveShortfalls = 0;
            bool retiredAnything = false;
            for (;;) {
                Attempt attempt;
                attempt.mode = mode;
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
                    // §7.3: one decode, inside this attempt's section, all three
                    // fields together. Nothing from it outlives the close.
                    attempt.binding = currentBinding();
                    counters.attempts.fetch_add(1, kRefcountAcquire);
                    st = runAttempt(lo, hi, value, attempt);
                    // The section is about to close. Nothing may still hold a
                    // claim: the set holds link-loaded node pointers, and a
                    // release issued after the close touches a node observed in a
                    // section that has ended (§3.2/§7.3).
                    assertNoClaimsHeld(attempt, "section close");
                }
                if (st == ApplyStatus::NeedsRecords) {
                    // §7.1's abandon-then-replenish. The section closed when the
                    // guard's scope ended above, which is the whole point: the
                    // remedy is a blocking grace-period call, and calling one
                    // from inside the attempt's own section waits on itself
                    // forever.
                    retiredAnything = retiredAnything || attempt.retired;
                    abandon(attempt);
                    replenishRecords();
                    ++consecutiveShortfalls;
                    // ─── At most ONE round, exactly (D-059 restores this) ───
                    //
                    // Every link in the chain is local to this CPU. The records
                    // the attempt wants belong to this CPU's pool; only this CPU
                    // draws from it; only this CPU retires them; `barrier` drives
                    // this CPU's own retirees to completion, so every one of them
                    // is pushed home. And the attempt cannot want more than the
                    // pool holds, because D-059's record budget IS the population
                    // — over-budget is `NeedsDecomposition`, handled elsewhere,
                    // not a shortfall. So the retried draw fits.
                    //
                    // A second consecutive shortfall is therefore not contention
                    // and no amount of retrying will fix it: it means a record
                    // did not come home, which is a leak, a stalled epoch, or a
                    // `homePool` pointing at nothing. Worth keeping exact —
                    // ITEM-084 had to weaken this to 64 rounds (D-056/D-057)
                    // because a shared reserve made a CPU's progress depend on
                    // another CPU's records, and a bound loose enough to tolerate
                    // that detects almost nothing.
                    assert(consecutiveShortfalls <= 1,
                           "radix: a DeferredRelease shortfall survived a barrier — every "
                           "record this attempt can want is this CPU's own and comes home "
                           "when its retirees are driven, so this is a record that is not "
                           "coming home at all (§7.1 / D-059)");
                    continue;
                }
                consecutiveShortfalls = 0;

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

                // DEC-060/DEC-071's operation-exit pump, AFTER the section closes.
                //
                // ─── What it does not achieve, corrected (R-19) ─────────────
                //
                // This used to say the pump makes the just-filled bag "stop being
                // the open one and become drainable by any CPU's later pump". It
                // does not: **`tryAdvance` never seals a bag.** Sealing is the
                // owner's own act — a rotation on this CPU's NEXT retire (when it
                // finds the open bag's tag stale), its own `barrier`, or
                // `drainAllQuiescent` at teardown. An Open bag is unclaimable by
                // anyone else, by I13, and `rcu_idle_slots_sealed_bags_are_drained_
                // by_other_cpus` pins exactly that: the open bag is the residue.
                //
                // What the pump genuinely buys is the epoch advance, and that is
                // worth two things. Everything ALREADY sealed on any CPU becomes
                // reclaimable sooner, which is the stealing benefit. And this CPU's
                // open bag is left with a stale tag, so its next retire rotates it
                // out rather than accumulating into it.
                //
                // So the honest exposure bound is "one operation's retirees until
                // this CPU's next retire, or until teardown" — not "until the next
                // pump anywhere". Narrower than nothing, wider than it read.
                if (retiredAnything) (void)kernel::rcu::tryAdvance(*domain);
                return st;
            }
        }

        // ─── §6.5 decomposition ────────────────────────────────────────────
        //
        // Run this range as one unit; if it does not fit the budget, decompose.
        // Every entry into the tree goes through here, so a sub-range produced by
        // decomposition is treated exactly like a top-level operation — which is
        // §6.5's "the sub-range inside that child is a sub-operation whose own
        // detachment count is summed and budget-checked, decomposing at its own
        // site if over".
        [[nodiscard]] ApplyStatus applyOrDecompose(uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value,
                                                   ApplyMode mode, unsigned depth) {
            const ApplyStatus st = runToCompletion(lo, hi, value, mode);
            if (st != ApplyStatus::NeedsDecomposition) return st;
            // A decomposed OnlyIfEmpty is deliberately NOT supported, and the
            // assert is the forcing function rather than a silent downgrade to
            // Overwrite. Decomposition applies the range in several independent
            // units, so "the whole range was empty" stops being a property any
            // one unit can establish — an early unit installs, a later one finds
            // its slice occupied, and the operation has half-placed a mapping
            // with no way back. Probed placement never needs it: it picks its own
            // range into space it just observed empty, so there is no populated
            // subtree to blow the detachment budget.
            assert(mode == ApplyMode::Overwrite,
                   "radix: an OnlyIfEmpty operation needed decomposition — placement picks "
                   "its range into observed-empty space, so this means the range was not "
                   "empty, or a caller used OnlyIfEmpty for something other than placement");
            return decompose(lo, hi, value, depth);
        }

        // The **site node**: "the deepest node containing the operation's *entire
        // range*" (§6.5, DEC-077). Unique and well-defined for a contiguous
        // range, and it necessarily contains every fully-covered subtree.
        //
        // §6.5 records that two earlier predicates were wrong, and both are worth
        // keeping visible because each looks more natural than this one:
        //
        //   - "deepest node beneath which the subtrees sum over budget" is
        //     upward-closed, so its minimal elements need not be unique — an
        //     ordinary multi-GiB MAP_FIXED reaches sixteen incomparable
        //     candidates, each of which drops the other fifteen's spans.
        //   - "deepest common ancestor of the fully-covered subtrees" need not
        //     contain the range at all: with every covered subtree inside one
        //     dense child and the range's tail in a sparse sibling, the DCA is
        //     the dense child and the tail's slots are assigned to no unit — a
        //     decomposed MAP_FIXED that maps only part of its range.
        //
        // Anchoring to the RANGE closes both. The descent is therefore driven
        // purely by the range: descend while the whole range still fits inside a
        // single slot AND that slot holds a node to descend into.
        struct SiteRef {
            NodeRef  node;
            unsigned level    = 0;
            uint64_t nodeBase = 0;
        };

        [[nodiscard]] SiteRef findSiteLocked(const RootBinding& bind,
                                             uint64_t lo, uint64_t hi) const {
            SiteRef s{bind.root, bind.level, bind.base};
            for (;;) {
                const unsigned first = slotIndexFor(s.level, s.nodeBase, lo);
                const unsigned last  = slotIndexFor(s.level, s.nodeBase, hi);
                // The range straddles two slots here, so no deeper node contains
                // it: this node is the site.
                if (first != last) return s;
                const uint64_t word = s.node.slot(first).load(kSlotLoad);
                // A leaf or an empty slot has no node beneath it to be the site.
                if (!Codec::isChild(word)) return s;
                s.nodeBase += uint64_t{first} * Geo<G>::slotSpan(s.level);
                s.node = NodeRef(Codec::decodeChild(word));
                s.level++;
            }
        }

        // "the operation **decomposes per child slot at the site node**", and the
        // decomposition processes "**every slot the operation's range intersects**
        // at it — empty ones included — ascending VA".
        //
        // The six per-slot rows §6.5 enumerates are NOT re-implemented here. Each
        // intersected slot is handed to an ordinary attempt over its clipped
        // sub-range, and the ordinary dispatch produces exactly the row §6.5
        // names for it:
        //
        //   operation-covered leaf      → OverwriteLeaf / ClearSlot
        //   fully-intersected empty     → WriteLeaf for a map; NoOp for a clear
        //                                 (no store, NO COUNT DECREMENT — §6.3's
        //                                 clear row is for OCCUPIED slots, and the
        //                                 spurious fetch_sub is silent in release)
        //   partially-intersected empty → NoOp for a clear; for a map an
        //                                 edge-partial sub-range leaf, or §6.1's
        //                                 shortfall spine where the boundary is
        //                                 inexpressible at this level (writing the
        //                                 full span would map addresses the caller
        //                                 never asked for)
        //   partially-covered leaf      → the ordinary edge/subdivision rows
        //   partially-covered child     → DescendIntoChild — never subdivision,
        //                                 which would detach a live interior node
        //                                 — and recurses
        //   fully-covered child         → DetachChild if its subtree fits the
        //                                 budget, otherwise recurses, the
        //                                 recursion landing on that child as its
        //                                 own site
        //
        // Reusing the dispatch is what makes "a decomposed MAP_FIXED over a range
        // with holes maps exactly what the unit path would — no more, no less"
        // true by construction rather than by a parallel table that has to be
        // kept in agreement with §6.3.
        //
        // Note what is deliberately NOT asserted here. §6.5 is explicit that
        // "one detachment site per unit" and "claims <= one subtree" are asserts
        // that FIRE ON LEGAL EXECUTIONS — a fitting sub-range's several small
        // subtrees are legitimately one unit. The budget is cumulative per
        // attempt, and the claim set's fixed capacity is the structural check.
        [[nodiscard]] ApplyStatus decompose(uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value,
                                            unsigned depth) {
            // The per-slot recursion strictly descends, so tree depth bounds it.
            // The final unit below can re-enter at the same range if a concurrent
            // operation rebuilt under the site, which is legal but must not be
            // able to spin: this is the forcing function for that.
            assert(depth <= 2 * (G.levelCount + 2),
                   "radix: decomposition recursion is not descending (§6.5)");

            SiteRef site;
            RootBinding bind;
            {
                kernel::rcu::ReadGuard guard(*domain);
                bind = currentBinding();
                site = findSiteLocked(bind, lo, hi);
            }

            const uint64_t span  = Geo<G>::slotSpan(site.level);
            const unsigned first = slotIndexFor(site.level, site.nodeBase, lo);
            const unsigned last  = slotIndexFor(site.level, site.nodeBase, hi);

            counters.decompositions.fetch_add(1, kRefcountAcquire);

            // "Every intersected slot is assigned this way, none skipped."
            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = site.nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                ApplyStatus st = runToCompletion(clipLo, clipHi, value,
                                                 ApplyMode::Overwrite);
                if (st == ApplyStatus::NeedsDecomposition) {
                    // Recursing on the SAME range would not terminate. It cannot
                    // happen: the site is the deepest node containing the range,
                    // so either the range straddles several of its slots (each
                    // sub-range is strictly smaller) or the single slot it lands
                    // in holds no child — and a slot with no child beneath it has
                    // no subtree to blow the detachment budget. A sub-range that
                    // is still over budget with nowhere to descend is a geometry
                    // defect, not a decomposable shape.
                    //
                    // **The same argument covers D-059's record budget**, and it
                    // is worth spelling out because the resource is different. A
                    // range inside one childless slot writes exactly that ONE
                    // slot: the walk from the cluster root reaches it through
                    // `DescendIntoChild` rows, which draw nothing, and there is
                    // nothing below it to draw for. One written slot displaces at
                    // most one distinct `Mapping`, so it needs at most one record
                    // — and `create` refuses a population below one. So the
                    // record budget can never be the cause of an unsplittable
                    // over-budget range either, and record-driven decomposition
                    // converges on the same bound as detachment-driven.
                    assert(!(clipLo == lo && clipHi == hi),
                           "radix: decomposition cannot make the range smaller — the site "
                           "is not the deepest node containing it (§6.5)");
                    st = decompose(clipLo, clipHi, value, depth + 1);
                }
                if (st != ApplyStatus::Ok) return st;
            }

            // ─── The final coarse-replacement unit ─────────────────────────
            //
            // "**The coarse-value replacement of the flattened site happens only
            // when the site itself is fully covered, is not the cluster root, and
            // the operation writes a value**" — then a final unit applies the
            // fully-covered-child row at its PARENT, and the terminal state is the
            // unit path's single coarse leaf.
            //
            // Each of the three conditions excludes a different disaster:
            //
            //   - A **clearing** operation gets no final unit: §6.4's ordinary
            //     reclamation-candidate path is the terminal mechanism, and
            //     wiring both would RETIRE THE SITE TWICE.
            //   - A **cluster-root** site gets none: its parent is a bucket word
            //     with no state word to claim, and invariant 16 forbids marking or
            //     unlinking the cluster root before teardown. An emptied cluster
            //     root simply stays, exactly as §6.4's walk-termination prescribes.
            //   - A **partially-covered** site is never replaced: its per-slot
            //     results stand. Wiping it wholesale would unmap the survivors in
            //     its partially-covered slots — the hole-freedom violation
            //     invariant 30 forbids.
            //
            // Since the site is by construction the deepest node CONTAINING the
            // range, "the site is fully covered" is exactly "the range is the
            // site's whole span".
            const uint64_t siteSpan = Geo<G>::nodeSpan(site.level);
            const bool siteFullyCovered =
                (lo <= site.nodeBase) && (hi >= site.nodeBase + siteSpan - 1);
            // Against the binding THIS decomposition descended from, not
            // against a tree member: a concurrent growth makes the two differ,
            // and the site was found under `bind`.
            const bool siteIsClusterRoot = (site.node == bind.root);

            if (value != nullptr && siteFullyCovered && !siteIsClusterRoot) {
                // The site's slots now all name `value`, so the subtree behind
                // this row is the site alone — one node, comfortably under the
                // budget — and the row taken at the parent is DetachChild.
                return applyOrDecompose(lo, hi, value, ApplyMode::Overwrite, depth + 1);
            }
            return ApplyStatus::Ok;
        }

        // ─── Structural walk, for validators and tests ─────────────────────
        //
        // Visits every NODE as `fn(level, nodeBase, node)`, in ascending VA;
        // slots are the caller's to iterate. `fn` returns void. Kept here rather
        // than in the harness because the quiesced-tree
        // validators of §11 need it and because a walk written against the
        // node's real layout is the one that catches a layout regression.
        template <typename F>
        void walk(F&& fn) const {
            const RootBinding b = quiescedBinding();
            if (b) walkNode(b.root, b.level, b.base, fn);
        }

        // Live node count, for the accounting targets.
        [[nodiscard]] size_t nodeCount() const {
            size_t n = 0;
            const RootBinding b = quiescedBinding();
            if (b) countNodes(b.root, b.level, n);
            return n;
        }

        // ─── The seam (§5.5) ───────────────────────────────────────────────
        //
        // The core layer's ONE piece of knowledge about where its root comes
        // from: somebody produces a {root, level, base} triple, atomically,
        // inside the section that is about to use it. Every caller below invokes
        // this from inside its own `ReadGuard` and holds the result no longer.
        //
        // A tree over a fixed span — Phase 1 and 2's shape, and the shape the
        // model check and the decomposition suite are written against — answers
        // from its own members. A tree that is a CLUSTER answers by reading its
        // bucket word through `protectWord` and decoding it, which is why this is
        // a function and not three field reads scattered through the machinery:
        // swapping it is the whole of what growth changes here.
        [[nodiscard]] RootBinding currentBinding() const {
            if (bucketTable == nullptr) return RootBinding{rootRef, level0, baseVA};
            // §3.1: the root-bucket word goes through the SAME protectWord
            // primitive as every slot link, with the bucket codec's decode —
            // "there is no exception". §7.3: one load, all three fields, and
            // nothing from it outlives the caller's section.
            const uint64_t w =
                kernel::rcu::protectWord(*domain, buckets()->entries[bucketIndex]);
            const auto d = BucketCodecT::decode(w, bucketIndex);
            return RootBinding{NodeRef(d.root), d.level, d.base};
        }

    private:
        // Forward-declared: `buildSubtree` and the claim helpers take it by
        // reference, and they are defined above its definition.
        struct Attempt;

        // A tree over a fixed span (Phase 1/2, the model check, the
        // decomposition suite) leaves `bucketTable` null and answers from these.
        // A tree that is a CLUSTER leaves them unset and answers from its bucket
        // word — which is the ONLY thing that differs between the two, and the
        // reason this is one branch in one function rather than a type split.
        NodeRef  rootRef{};
        unsigned level0 = 0;
        uint64_t baseVA = 0;

        // Raw since D-051 moved the root page to pinned storage, whose mapping
        // never changes and so owes no freshness call — on what is the hottest
        // read in the tree. `ClusterTable::buckets` carries the `static_assert`
        // that keeps this an argument about the storage rather than about this
        // comment; the page reaching a tree is always one that table handed out.
        [[nodiscard]] BucketTable* buckets() const { return bucketTable; }

        BucketTable* bucketTable = nullptr;
        size_t       bucketIndex = 0;
        kernel::rcu::Domain* domain = nullptr;
        // §7.1's per-CPU DeferredRelease population. Per ADDRESS SPACE, not per
        // tree: a tree is one cluster and an address space has many, so the
        // pointer is bound at init and the storage belongs to the control block.
        DeferredReleasePools* releasePools = nullptr;
        TreeStats counters{};
        // Jitter state for the retry backoff. Shared rather than per-CPU because
        // it only needs to decorrelate, and the CPU id is mixed in at use.
        //
        // A member initializer again. This was briefly seeded from a `CoreTree()`
        // body, because a non-empty `Atomic{...}` initializer used to be a SEQ_CST
        // store and this member is constructed once per lookup along with the rest
        // of the tree value. `Atomic(T)` is `constexpr` and non-atomic now, so the
        // initializer is free and the constructor body it forced is gone.
        Atomic<uint64_t> backoffSequence{0x243F6A8885A308D3ull};
        // DEC-060: whether this operation retired anything, so operation exit
        // knows whether to pump. Pumping unconditionally would be harmless but
        // dishonest — the site exists to stop a just-filled bag sitting OPEN on
        // a CPU that may never mutate this domain again, and an operation that
        // retired nothing has no bag to advance.
        bool retiredThisOperation = false;

        // `Geo<G>` rather than the free derivations, and for a measured reason
        // (D-076): `level` here is a descent's loop variable, so
        // `slotSpanBits(G, level)` compiles to a LOOP over `levelBits[]` and
        // this one-line function cost about thirty instructions per step. See
        // Geometry.h — the table is those same derivations, folded.
        static unsigned slotIndexFor(unsigned level, uint64_t nodeBase, uint64_t va) {
            return static_cast<unsigned>(((va - nodeBase) >> Geo<G>::slotSpanBits(level)) &
                                         (Geo<G>::valence(level) - 1));
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
        static void* buildSubtree(unsigned childLevel, uint64_t base, const SegmentPlan& plan,
                                  const Attempt& a) {
            // §11 / §9: "no claim is held across an ALLOCATION or a section
            // close". §6.1's "allocate before you claim" is what makes a
            // shortfall cost a retry rather than an in-place allocation, and an
            // allocation under a held claim is also the blocking-while-holding
            // shape §3.2 forbids outright.
            assertNoClaimsHeld(a, "buildSubtree");
            void* raw = Ops::alloc(childLevel, /*initialRefcount=*/0);
            if (!raw) return nullptr;
            NodeRef node(raw);

            const uint64_t childSpan = Geo<G>::slotSpan(childLevel);
            const unsigned n = Geo<G>::valence(childLevel);
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
                void* grand = buildSubtree(childLevel + 1, slotBase, local, a);
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
            const unsigned n = Geo<G>::valence(level);
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
            const unsigned n = Geo<G>::valence(level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isLeaf(w)) {
                    // Same obligation on the WRITER side: these records were
                    // read out of slot words and this CPU may never have touched
                    // them before.
                    acquireMappingRef(VMSubstrate::SafePtr<Mapping>(
                        static_cast<Mapping*>(Codec::decodeLeaf(w))));
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
            const unsigned n = Geo<G>::valence(level);
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) {
                    releaseSubtree(NodeRef(Codec::decodeChild(w)), level + 1);
                }
            }
            Ops::template runDeleter<Codec>(level, node);
        }

        // ─── §7.1 / DEC-068: drawing, returning and retiring records ───────
        //
        // The draw happens during RE-DISPATCH, and the site is forced rather
        // than chosen. It cannot be in commit: a draw can fail, and nothing
        // after the commit boundary may (§6.1). It should not be in the read
        // pass either: that pass runs unclaimed, so its rows are advisory and it
        // would over-draw on every row a concurrent writer then changes.
        // Re-dispatch is the one place that walks the frozen rows before the
        // boundary — which is also why the accounting is COMPLETE there and
        // commit does no counting of its own.
        //
        // ─── The two ways a draw can fail, which need opposite remedies ────
        //
        // `Shortfall` means the records exist but are in flight — the pool is
        // momentarily empty. The remedy is a grace period, and the retried
        // attempt is identical.
        //
        // `OverBudget` means the attempt wants more records than a pool holds, so
        // no grace period helps and retrying is a livelock. The remedy is to make
        // the OPERATION smaller: §6.5 decomposition. (D-059.)
        //
        // Conflating them is the defect ITEM-084 was built around rather than
        // fixed: it answered "the attempt wants more than the pool has" with "get
        // more records", which needs a shared reserve, which couples one CPU's
        // progress to another's.
        enum class DrawResult : uint8_t { Ok, Shortfall, OverBudget };

        [[nodiscard]] DrawResult deferMappingRelease(Mapping* m, Attempt& a) {
            assert(m != nullptr, "radix: a deferred release of no record");
            assert(a.phase == Attempt::Phase::Planning,
                   "radix: a release record drawn after the commit boundary — the draw "
                   "can fail, and nothing after the boundary may (§6.1)");
            // §7.1: record-drawing operations are exactly the DISPLACEMENT
            // operations (mmap into occupied ranges, munmap, mprotect splits),
            // which run in syscall context and never in #PF. Asserted at the
            // draw site because the remedy for a shortfall is `barrier`, whose
            // context mask is the strict one — so a draw from a fault path is
            // not merely unusual, it is a path with no legal way out.
            assert(!kernel::interrupts::currentCpuInterruptDepths().inAnyInterruptContext(),
                   "radix: a DeferredRelease draw from interrupt or #PF context — the "
                   "shortfall remedy is `barrier`, which is illegal there (§7.1)");

            // §7.1's "one record per DISTINCT Mapping ... carrying the aggregate
            // deferred decrement". The linear walk is over the records THIS
            // attempt holds, which is bounded by the edge sum and is one or two
            // in every shape that occurs: the common `munmap` displaces one
            // record from many slots.
            for (DeferredRelease* r = a.heldReleases; r != nullptr;
                 r = r->next.load(kPrivateInit)) {
                if (r->mapping == m) { r->delta++; return DrawResult::Ok; }
            }

            // ─── The budget, checked BEFORE the pool is touched (D-059) ─────
            //
            // Ahead of the `pop` so an over-budget attempt consumes nothing: it is
            // about to be discarded and decomposed, and a record drawn here would
            // only be handed straight back. Also ahead of it because the two
            // conditions must not be able to mask each other — an attempt that is
            // over budget AND meets a momentarily empty pool must report
            // `OverBudget`, since a grace period would not help it.
            //
            // The budget is the per-CPU population, and that identity is what
            // makes the shortfall path terminate in one round. See
            // `DeferredReleasePools::recordBudget`.
            if (a.heldReleaseCount >= releasePools->recordBudget()) {
                return DrawResult::OverBudget;
            }

            const arch::ProcessorID cpu = arch::getCurrentProcessorID();
            if (!a.drawCpuValid) { a.drawCpu = cpu; a.drawCpuValid = true; }
            assert(a.drawCpu == cpu,
                   "radix: an attempt drew DeferredRelease records on two different CPUs — "
                   "the records' home pool and the CPU that will barrier for them have "
                   "diverged, so §7.1's replenish cannot bring them home (D-070/R-12)");
            DeferredReleasePool& pool = releasePools->forCpu(cpu);
            DeferredRelease* r = pool.pop();
            if (r == nullptr) {
                pool.shortfalls.fetch_add(1, kPoolAccounting);
                return DrawResult::Shortfall;
            }
            pool.draws.fetch_add(1, kPoolAccounting);

            // `pop` paid the record's first-touch freshness, so these writes go
            // through a fresh mapping (§7.1 — the draw is a cross-CPU first
            // touch of possibly-recycled arena memory, with no deleter in
            // sight).
            r->mapping = m;
            r->delta   = 1;
            r->next.store(a.heldReleases, kPrivateInit);
            a.heldReleases = r;
            a.heldReleaseCount++;
            assert(a.heldReleaseCount <= releasePools->recordBudget(),
                   "radix: an attempt holds more DeferredRelease records than the budget "
                   "permits — the gate above is the only way to draw, so this means the "
                   "held list was extended somewhere else (§7.1 / D-059)");
            return DrawResult::Ok;
        }

        // Abandonment: every drawn record goes home, and it does so BEFORE the
        // section closes (§7.3). Uniformity with the claim release rather than a
        // necessity — the load-bearing edge for records is the thread's own
        // destruction join — but it is free, and it keeps every record motion
        // inside the protocol the validators watch.
        static void returnHeldRecords(Attempt& a) {
            DeferredRelease* r = a.heldReleases;
            while (r != nullptr) {
                DeferredRelease* const next = r->next.load(kPrivateInit);
                r->mapping = nullptr;
                r->delta   = 0;
                r->next.store(nullptr, kPrivateInit);
                r->homePool->push(r);
                r = next;
            }
            a.heldReleases     = nullptr;
            a.heldReleaseCount = 0;
        }

        // Commit: each record becomes its own retire subject. After the whole
        // commit walk, so every publish that stopped a slot naming its `Mapping`
        // has already happened — §6.1's phase order (all marks, then the +1s,
        // then all publishes, then retires, then releases) with the record
        // retires standing in for the releases.
        void retireHeldRecords(Attempt& a) {
            assert(a.phase == Attempt::Phase::Committing,
                   "radix: a release record retired before the commit boundary (§11)");
            // The other end of D-070's pinning check: these retires bind to the
            // CALLING CPU's RCU slot, so they must be the CPU that drew them or the
            // records come home to a pool nobody will barrier for.
            assert(!a.drawCpuValid || a.drawCpu == arch::getCurrentProcessorID(),
                   "radix: records drawn on one CPU are being retired on another — the "
                   "retire binds to the caller's slot, so §7.1's replenish would wait on "
                   "a barrier that never covers them (D-070/R-12)");
            // ITEM-084's sample, taken at the commit boundary because that is
            // the peak an attempt actually held — abandonment returns records,
            // so a retried operation's demand is per attempt, not cumulative.
            noteDrawCount(a.heldReleaseCount);
            noteDetachSize(a.detachNodes);
            DeferredRelease* r = a.heldReleases;
            while (r != nullptr) {
                DeferredRelease* const next = r->next.load(kPrivateInit);
                r->next.store(nullptr, kPrivateInit);
                assert(r->delta != 0,
                       "radix: retiring a record with a zero delta — it was drawn and "
                       "never charged, which leaks the record and loses nothing else, so "
                       "nothing downstream would notice");
                kernel::rcu::retire<DeferredRelease, &DeferredRelease::head,
                                    &deleteDeferredRelease>(*domain, r);
                a.retired = true;
                r = next;
            }
            a.heldReleases     = nullptr;
            a.heldReleaseCount = 0;
        }

        // §7.1's replenish, run BETWEEN attempts and outside any section.
        //
        // The primitive is **`barrier`, never `synchronize`**, and the reason is
        // not stylistic: `synchronize` promises only that a grace period has
        // elapsed and does NOT seal the caller's still-open bag — which is
        // precisely where the missing records are. `barrier` seals, rotates and
        // drives this CPU's own retirees, whose deleters push every one of them
        // home; since only the owner draws from its own pool, the retried
        // attempt's draw then succeeds deterministically.
        //
        // Calling either from inside the attempt's own section would wait on
        // itself forever (§10's DEC-046 genre), which is why the caller abandons
        // first and this is not an optimisation.
        void replenishRecords() {
            assert(!kernel::interrupts::currentCpuInterruptDepths().inAnyInterruptContext(),
                   "radix: record replenish from interrupt or #PF context — `barrier` "
                   "carries the strict mask");
            DeferredReleasePool& pool =
                releasePools->forCpu(arch::getCurrentProcessorID());

            // The pump first, because it is cheap and sometimes enough: a bag
            // that another CPU has already left drainable costs no grace period
            // to collect, and RCU's sweep is domain-wide (RCU-DEC-006 stealing).
            //
            // **Unconditional, both of them.** There used to be an
            // `if (pool.atFullPopulation()) return;` early return above this and
            // it was a reproduced livelock (D-058): the predicate asks whether the
            // pool holds all the records it OWNS, which under ITEM-084's small
            // pools read TRUE in exactly the state that produces a shortfall —
            // abandonment returns every drawn record before this runs, so the CPU
            // arrives holding precisely its native population. The gate fired, the
            // pump never ran, and the retry re-entered the identical state. There
            // is no timer-driven RCU pump in the kernel — every `tryAdvance` call
            // site is on an operation path — so nothing else recovers it, and in
            // release `assert` compiles out and it is an unbounded silent spin
            // inside `munmap`.
            (void)kernel::rcu::tryAdvance(*domain);
            (void)kernel::rcu::drain(*domain);

            // "If still short" — against the full population rather than against
            // emptiness, because a pool holding three records when the operation
            // needs five is not empty and is still short.
            //
            // With D-059's budget this predicate is finally EXACT rather than
            // conservative: the budget is the population, so "at full population"
            // and "enough for any attempt this tree will run" are the same
            // sentence. Under ITEM-084 they were not, and the gap was a redundant
            // barrier on every wide draw.
            if (!pool.atFullPopulation()) kernel::rcu::barrier(*domain);
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
            ClaimSet<G, DetachBudget> claims;

            // The root triple this attempt is working against, decoded ONCE
            // from one bucket-word load inside this attempt's section. Every
            // "is this the cluster root?" test below compares against THIS,
            // not against a tree member: under growth the two can differ, and
            // the answer that matters is the one the attempt's own descent was
            // computed from.
            RootBinding binding{};

            // ─── §11: "No irreversible step precedes the commit boundary" ───
            //
            // "Per-operation phase flag asserted at EVERY mark, slot write and
            // retire — not only the first publish, since THE MARKING VIOLATION IS
            // THE ONE WITH NO OBSERVABLE SYMPTOM."
            //
            // That last clause is the whole reason this is a flag rather than a
            // structural argument. A partially-applied mmap at least leaves an
            // observable wrong mapping; a subtree marked by an operation that then
            // backs off leaves no symptom at all until something tries to map that
            // range, forever (§6.1).
            enum class Phase : uint8_t { Planning, Committing };
            Phase phase = Phase::Planning;

            // ─── §11: "The mark is the last acquisition on its path" ────────
            //
            // Set by the first mark this attempt issues. Any acquisition after it
            // is the violation — the mark is irreversible, so a claim taken after
            // one has no way to unwind if it fails.
            //
            // Note this is deliberately about ACQUISITIONS, not about writes.
            // §11's exemption list ("the post-mark parent-slot store in subtree
            // detachment") exists because the NAIVE formulation guards writes, and
            // §6.5 says of that store: "it is not an acquisition after a mark, and
            // a naive 'mark is last' assert must exempt it explicitly". Guarding
            // writes would additionally fire on an ordinary legal execution — a
            // commit whose slot 3 detaches (marking) and whose slot 4 is a plain
            // leaf write — so the write-based form is not merely inconvenient,
            // it is false. See D-023.
            bool markIssued = false;

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

            // ─── §7.1 / DEC-068: the deferred releases this attempt holds ───
            //
            // An intrusive list through each record's `next` field rather than
            // an array, and that is not a micro-optimisation: the ceiling is the
            // EDGE SUM (≈230 under the kernel geometry), so an array would put
            // ~1.8 KiB on the stack of every operation, on top of the claim set,
            // for a worst case almost nothing reaches.
            //
            // Records are drawn during re-dispatch — before the commit boundary,
            // because nothing after the boundary may fail — and are either
            // retired (success) or returned to their home pool (abandonment).
            DeferredRelease* heldReleases    = nullptr;
            unsigned         heldReleaseCount = 0;

            // ─── The CPU this attempt's records belong to (D-070) ───────────
            //
            // D-059's termination argument reads "every link in the chain is local
            // to this CPU" as if by construction. It is by PINNING: the draw
            // re-reads `getCurrentProcessorID()` every time, and `retire`/`barrier`
            // bind to the calling CPU's slot. Under a scheduler that migrated a
            // thread mid-attempt, the drawn records' home pool and the barriering
            // slot would diverge, and the failure is R-1's shape — assert in debug,
            // unbounded spin inside `munmap` in release.
            //
            // No scheduler exists yet (R-12's premise), so rather than assert
            // scheduler predicates that are file-local to `RCU.cpp`, this records
            // where the first draw happened and every later draw checks against it.
            // That catches the divergence with nothing exported and no runtime cost
            // in release, and it turns a stated premise into a checked one — the
            // same move `claimsFullValence` made for R-9.
            arch::ProcessorID drawCpu{};
            bool              drawCpuValid = false;

            // Set only by a failed DRAW. `redispatchAgrees` returning false
            // otherwise means the tree moved under the claim set, which is an
            // ordinary retry; a shortfall is not, and its remedy is a blocking
            // replenish that must run between attempts and outside any section.
            // One setter, one reader — the flag exists so the recursive
            // re-dispatch can stay a plain bool.
            bool recordShortfall = false;

            // Set only by a draw refused for exceeding the record budget (D-059).
            // A SECOND flag rather than a shared "the draw failed" one, because the
            // two remedies are opposite and mutually exclusive: a shortfall retries
            // the identical attempt after a grace period, while this one must never
            // retry the identical attempt — the demand is a property of the shape,
            // so retrying is a livelock. Decomposing is what fixes it.
            bool recordsOverBudget = false;

            // §5.6's fused verify-is-install. `mode` is the caller's; `occupied`
            // is set by re-dispatch, under the claims, which is the only place
            // the answer is exact — the read pass runs unclaimed, so a range it
            // saw empty can fill before the claims land.
            ApplyMode mode     = ApplyMode::Overwrite;
            bool      occupied = false;

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

        ApplyStatus runAttempt(uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value, Attempt& a) {
            assert(a.binding,
                   "radix: an attempt against a bucket holding no cluster — creation is "
                   "the caller's to do first (§5.6), and it publishes an EMPTY root "
                   "precisely so that placing into it is an ordinary operation");
            assert(lo >= a.binding.base &&
                   hi < a.binding.base + Geo<G>::nodeSpan(a.binding.level),
                   "radix: the operation's range escapes the cluster's current span — the "
                   "caller owes a growToCover first. Growth only ever WIDENS, so a range "
                   "that fitted when the caller checked still fits here");

            // ─── Pass 1: read-only ─────────────────────────────────────────
            const ReadOutcome rr =
                readPass(a.binding.root, a.binding.level, a.binding.base, lo, hi, value, a);
            if (rr.status != ApplyStatus::Ok) return rr.status;

            // ─── Acquire, top-down (§6.8) ──────────────────────────────────
            a.claims.sortForAcquisition();
            if (!acquireAll(a)) {
                counters.claimConflicts.fetch_add(1, kRefcountAcquire);
                // Release INSIDE the section. §3.2/§7.3: the claim set is a set
                // of link-loaded node pointers and those do not survive a close,
                // so a release deferred to `abandon` — which runs after the
                // guard's scope ends — dereferences them outside the section that
                // observed them. See D-024.
                releaseAll(a.claims);
                returnHeldRecords(a);
                return ApplyStatus::Retry;
            }

            // ─── Re-dispatch (still before the boundary, may still fail) ───
            //
            // Re-run the dispatch at every held site against the now-frozen slot
            // words. A row that changed means the set the pass computed is
            // wrong; the answer is to discard and retry, never to extend the set
            // in place.
            if (!redispatchAgrees(a.binding.root, a.binding.level, a.binding.base,
                                  lo, hi, value, a)) {
                // Both unwind paths release and return everything INSIDE the
                // section (§7.3, D-024). They differ only in what the caller
                // does next: a changed row is retried after a backoff, a record
                // shortfall after a blocking replenish that cannot run here.
                releaseAll(a.claims);
                returnHeldRecords(a);
                if (a.occupied) {
                    // Not a retry: the answer is stable. Something is mapped
                    // there, and retrying would only re-discover it.
                    return ApplyStatus::Occupied;
                }
                // Before the shortfall, because it is the stabler answer of the
                // two. Only one of the flags can be set — the walk returns as
                // soon as either fires — so the order is determinism rather than
                // precedence, and it is written as an order so a later change
                // that lets both be set does not silently pick the wrong remedy.
                if (a.recordsOverBudget) {
                    // §6.5's own status, not a new one. The operation is too big
                    // for one attempt, which is exactly what the detachment
                    // budget already says with the same word, and the caller's
                    // decompose-then-retry machinery is already correct for it.
                    counters.recordBudgetOverruns.fetch_add(1, kRefcountAcquire);
                    return ApplyStatus::NeedsDecomposition;
                }
                if (a.recordShortfall) {
                    counters.recordShortfalls.fetch_add(1, kRefcountAcquire);
                    return ApplyStatus::NeedsRecords;
                }
                counters.redispatchChanges.fetch_add(1, kRefcountAcquire);
                return ApplyStatus::Retry;
            }

            // ─── The commit boundary ───────────────────────────────────────
            //
            // §6.4: the reclamation decision is fixed HERE, from the priors the
            // claims froze — a pure bottom-up computation over data the writer
            // already holds. Nothing in it can fail, and it runs BEFORE the
            // first mark.
            // ─── The commit boundary is CROSSED HERE ───────────────────────
            //
            // Everything above could still fail and unwind; nothing below can.
            // The flag exists so that "nothing irreversible precedes this line"
            // is checked at every mark, publish and retire rather than argued.
            a.phase = Attempt::Phase::Committing;
            commit(a.binding.root, a.binding.level, a.binding.base, lo, hi, value, a);

            // §7.1: the drawn records are retired here, after every publish that
            // stopped a slot naming their `Mapping`. Each is its own retire
            // subject — a `Mapping` cannot be one, because the relationship is
            // N:1 and two CPUs would enqueue the same intrusive linkage twice.
            retireHeldRecords(a);

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
                             uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = Geo<G>::slotSpan(level);
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
                        if (writes) plan.add(d.opRange, value.address());

                        void* built = buildSubtree(level + 1, slotBase, plan, a);
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
            const bool isRoot    = (node == a.binding.root);
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
            if (++a.detachNodes > DetachBudget) {
                noteDecomposition();
                return ApplyStatus::NeedsDecomposition;
            }
            if (!a.claims.addOrMerge(node, level, nodeBase, valenceMask(G, level),
                                     /*wholeNode=*/true)) {
                return ApplyStatus::NeedsDecomposition;
            }
            const unsigned n = Geo<G>::valence(level);
            const uint64_t span = Geo<G>::slotSpan(level);
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
        static bool acquireAll(Attempt& a) {
            ClaimSet<G, DetachBudget>& set = a.claims;
            for (size_t i = 0; i < set.count; i++) {
                auto& e = set.entries[i];
                // §11: "The mark is the last acquisition on its path."
                //
                // A mark is irreversible, so a claim taken after one has no way
                // to unwind if it fails — the operation would have to abort
                // having already poisoned a node it no longer holds, which is
                // §6.8's marked-but-linked disaster reached from the other side.
                assert(!a.markIssued,
                       "radix: an acquisition issued after a mark — the mark must be the "
                       "last acquisition on its path (§11)");
                // The claim set is never extended in place, so acquisition runs
                // strictly before the boundary.
                assert(a.phase == Attempt::Phase::Planning,
                       "radix: acquisition after the commit boundary — a claim set extended "
                       "in place (§6.1)");
                // DEC-046's once-per-node rule, enforced structurally: the set
                // merges by node, so reaching an already-issued entry means the
                // set is malformed.
                assert(!e.issued,
                       "radix: second fetch_or on one node in one acquisition phase — the "
                       "deterministic single-CPU self-livelock (DEC-046)");
                e.issued = true;
                set.fetchOrIssued++;

                // §11's progress row. The epochs must be sampled BEFORE the
                // fetch_or: the audit's soundness rests on establishing that a
                // peer's held set spanned OUR failure, which a sample taken
                // afterwards cannot show.
                // §6.8's total order, asserted directly. The order is what makes
                // the maximum-holder lemma's hypothesis hold on every
                // acquisition, so a violation here voids the progress argument
                // outright — and it is deterministic, where the progress assert
                // is necessarily probabilistic.
                //
                // Note this is the ORDER over held claims, not "each re-dispatch
                // site is strictly deeper", which §11 records as an assert that
                // fires on a legal execution.
                const AuditSite entrySite{true, e.level, e.nodeBase};
                assert(entrySite.greaterThan(maxHeldSite(set)),
                       "radix: claims acquired out of §6.8's order (level, then ascending "
                       "VA) — deadlock-freedom rests on it");

                AuditEpochSnapshot before;
                auditSnapshotEpochs(before);

                const ClaimOutcome oc = tryClaim(e.node.stateWord(), e.mask);
                if (!oc.acquired) {
                    auditFailedAcquisition(before, e.node, e.mask, oc.prior,
                                           maxHeldSite(set), entrySite);
                    return false;
                }
                e.held  = true;
                e.prior = oc.prior;
                auditPublish(set);
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
        static bool holdsWholeNode(ClaimSet<G, DetachBudget>& set, NodeRef node, unsigned level) {
            const auto* e = set.find(node);
            return e != nullptr && e->held && (e->mask & valenceMask(G, level)) ==
                                                  valenceMask(G, level);
        }

        static void retainClaim(ClaimSet<G, DetachBudget>& set, NodeRef node) {
            if (auto* e = set.find(node)) e->held = false;
        }

        // `holdsWholeNode` minus the `held` bookkeeping, and the distinction is
        // real: `retainClaim` clears `held` at the mark, while the claim BIT stays
        // set in the node's state word for the rest of the commit. So after marking,
        // `holdsWholeNode` is false and the PHYSICAL precondition that
        // `kClaimedSlotLoad` depends on — every slot of this node frozen by a bit
        // this attempt owns — is still true.
        //
        // Exists so that precondition is CHECKED at each of the three sites that
        // depend on it rather than asserted in a comment, which is what let R-9's
        // two mixed-claim sites use the constant unnoticed.
        static bool claimsFullValence(ClaimSet<G, DetachBudget>& set, NodeRef node,
                                      unsigned level) {
            const auto* e = set.find(node);
            return e != nullptr &&
                   (e->mask & valenceMask(G, level)) == valenceMask(G, level);
        }

        // ─── The three commit-phase gate asserts (§11) ─────────────────────
        //
        // Every mark in the tree goes through here, on all three paths. Three
        // separate §11 targets meet at this one site, which is why it is a
        // funnel rather than three asserts scattered over the mark sites:
        //
        //   "The mark is reachable from every path"  — the writer must hold the
        //   node's FULL VALENCE MASK. §11: "This must hold on all three paths
        //   with no exemption — reclamation, detachment AND teardown. An
        //   exemption for teardown is the failure mode: it removes the check
        //   from the paths where it is load-bearing, and it is what an
        //   implementer will reach for the first time the assert fires on a
        //   process exit."
        //
        //   "No irreversible step precedes the commit boundary" — a mark is the
        //   subtler of the two irreversible steps, and the one with no
        //   observable symptom when it goes wrong.
        //
        //   "The mark is never set from a retire callback" — a deleter runs
        //   outside any section and holds no claim, so the valence-mask assert
        //   above is what actually catches it; the phase flag catches it too,
        //   since a deleter carries no attempt in commit phase.
        static void markUnderClaim(NodeRef node, unsigned level, Attempt& a) {
            assert(a.phase == Attempt::Phase::Committing,
                   "radix: a mark issued before the commit boundary — the one irreversible "
                   "step whose violation has no observable symptom (§11)");
            assert(holdsWholeNode(a.claims, node, level),
                   "radix: a mark taken without the node's full valence mask (invariant 19, "
                   "no exemption on any of the three paths)");
            markDying(node.stateWord());
            a.markIssued = true;
        }

        // A publish into a slot of a LIVE node. Every commit-phase store goes
        // through here so §11's "asserted at every slot write" is structural
        // rather than a count of copies that has to stay right.
        //
        // Deliberately NOT asserted here: `!a.markIssued`. Mark-last is a
        // property of ACQUISITIONS, and a publish after a mark is ordinary and
        // legal — a commit whose slot 3 detaches (marking) and whose slot 4 is a
        // plain leaf write does exactly that. See `publishAfterMark` for the site
        // §11 singles out, and D-023 for why the write-based formulation is false
        // rather than merely inconvenient.
        static void publish(NodeRef node, unsigned slot, uint64_t word, Attempt& a) {
            assert(a.phase == Attempt::Phase::Committing,
                   "radix: a slot published before the commit boundary (§11)");
            assert(holdsSlot(a.claims, node, slot),
                   "radix: publishing into a slot this attempt holds no claim bit for");
            node.slot(slot).store(word, kSlotPublish);
        }

        // The reclamation unlink. This parent slot carries no claim bit of ours
        // and is not supposed to: "descending THROUGH a slot takes no claim; only
        // writing one does — a child-pointer slot cannot legitimately change
        // except by reclamation or detachment, and both acquire the CHILD's state
        // word, which a writer working inside that child already holds. The
        // interlock lives in the child, not in the parent slot" (Claim.h).
        //
        // So the precondition here is the CHILD's whole-node claim, asserted by
        // the `markUnderClaim` immediately above every call. A separate entry
        // point rather than a `word == 0` escape in `publish`, which would also
        // wave through every ordinary ClearSlot.
        static void publishUnderChildClaim(NodeRef node, unsigned slot, uint64_t word,
                                           Attempt& a) {
            assert(a.phase == Attempt::Phase::Committing,
                   "radix: a slot published before the commit boundary (§11)");
            node.slot(slot).store(word, kSlotPublish);
        }

        // §6.5's named exemption, and §11's: "exempting the POST-MARK PARENT-SLOT
        // STORE in subtree detachment". The store happens after the marking phase
        // and is a publish under a bit claimed much earlier — "it is not an
        // acquisition after a mark, and a naive 'mark is last' assert must exempt
        // it explicitly" (§6.5).
        //
        // It is a distinct entry point rather than a comment so the exemption is
        // visible in the code, and so Phase 3's second exempt site — teardown's
        // final per-cluster bucket CAS (DEC-100) — has an obvious home.
        static void publishAfterMark(NodeRef node, unsigned slot, uint64_t word, Attempt& a) {
            assert(a.markIssued,
                   "radix: publishAfterMark used at a site that has issued no mark — the "
                   "exemption is for detachment's parent-slot store, not a general escape");
            publish(node, slot, word, a);
        }

        // Debug-only forcing function for D-013. Every slot `commit` mutates must
        // be one this attempt reserved a claim bit for in the read pass —
        // "descending THROUGH a slot takes no claim; only writing one does"
        // (Claim.h). Two writers publishing into the same unclaimed slot is the
        // shape that produces both §5.3's occupancy over-count and a `Mapping`
        // refcount underflow, so assert it at the write rather than inferring it
        // from the wreckage.
        static bool holdsSlot(ClaimSet<G, DetachBudget>& set, NodeRef node, unsigned slot) {
            const auto* e = set.find(node);
            return e != nullptr && e->held && (e->mask & (uint64_t{1} << slot)) != 0;
        }

        static void releaseAll(ClaimSet<G, DetachBudget>& set) {
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
            auditPublish(set);
        }

        // This attempt's lexicographic maximum HELD site, in §6.8's order. Held,
        // not merely recorded: an entry we planned but have not acquired is not
        // part of the lemma's "writer-held" relation.
        static AuditSite maxHeldSite(const ClaimSet<G, DetachBudget>& set) {
            AuditSite max;
            for (size_t i = 0; i < set.count; i++) {
                const auto& e = set.entries[i];
                if (!e.held) continue;
                const AuditSite s{true, e.level, e.nodeBase};
                if (s.greaterThan(max)) max = s;
            }
            return max;
        }

        // ─── Re-dispatch ───────────────────────────────────────────────────
        //
        // Every site is now claim-frozen, so re-running the dispatch is exact.
        // §11: "Re-dispatch is re-run, not re-read" — the check is on the ROW,
        // not on a value comparison, because a slot can change in a way a value
        // comparison notices while the row is unchanged, and vice versa.
        bool redispatchAgrees(NodeRef node, unsigned level, uint64_t nodeBase,
                              uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = Geo<G>::slotSpan(level);
            const unsigned first  = slotIndexFor(level, nodeBase, lo);
            const unsigned last   = slotIndexFor(level, nodeBase, hi);

            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                // ACQUIRE, not RELAXED (R-9). This loop covers slots the attempt
                // holds and slots it merely descends through, and only the former
                // are ordered by the claiming `fetch_or` — see kAttemptSlotLoad.
                const uint64_t word = node.slot(i).load(kAttemptSlotLoad);
                const DispatchResult d =
                    dispatchSlot<G, Codec>(word, level, slotBase, clipLo, clipHi, writes);

                // §6.1: "A row that changed means the set the pass computed is
                // wrong; the answer is to discard and retry, never to extend the
                // set in place."
                //
                // THIS is that check, and its absence was D-013. Re-running the
                // dispatch is only half of it — the other half is comparing the
                // row against what the pass actually reserved. The claim bit is
                // the faithful expression of that comparison: the read pass takes
                // bit `i` for exactly the rows that store to slot `i`, so a row
                // that now writes a slot with no bit is precisely a row that
                // changed since the pass.
                //
                // Both directions of the change are real and both were observed:
                // a slot the pass saw as EMPTY (NoOp, no bit) that a concurrent
                // writer filled, which now dispatches to a writing row; and a slot
                // the pass saw as a LEAF that a concurrent writer subdivided,
                // which now descends into a child the pass never enumerated and
                // therefore never claimed anywhere.
                //
                // Descent and no-op are exempt because neither stores to slot `i`
                // here — descent's own writes are checked by the recursion, and
                // its one parent-slot store (reclamation) is interlocked on the
                // CHILD's whole-node claim instead, which commit checks there.
                if (d.action != DispatchAction::NoOp &&
                    d.action != DispatchAction::DescendIntoChild &&
                    !holdsSlot(a.claims, node, i)) {
                    return false;
                }

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

                // ─── §5.6 / DEC-007: the fused emptiness verdict ────────────
                //
                // Judged HERE and nowhere else, because here is the only place
                // it is exact: every slot this operation will write is claimed,
                // so its word is frozen and cannot fill between this test and
                // the commit. The read pass's view is advisory by construction.
                //
                // The question is whether **the requested range** is mapped, not
                // whether the slot is untouched — and the difference is most of
                // the cluster. A C1 slot holding a leaf over its first 64 KiB
                // has 15 more granules free; the row there is `Subdivide`, which
                // ADDS alongside the survivor rather than displacing it. Judging
                // by "the word is non-empty" rejects all of them and caps a
                // 512-granule cluster at 32 placements — one per slot, with the
                // scan reporting the cluster full while 94% of it is empty.
                //
                // So: a leaf is occupancy only where it OVERLAPS. A child word
                // that did not dispatch to a descend is a fully-covered subtree
                // and is treated as occupied outright — conservative for
                // D-030's stranded empty node, and conservative is the right
                // direction here, since the cost is a probe that moves on.
                if (a.mode == ApplyMode::OnlyIfEmpty &&
                    d.action != DispatchAction::DescendIntoChild) {
                    if (Codec::isLeaf(word)) {
                        uint64_t leafLo = 0, leafHi = 0;
                        Codec::absoluteRange(word, slotBase, level, leafLo, leafHi);
                        if (!(leafHi < clipLo || leafLo > clipHi)) {
                            a.occupied = true;
                            return false;
                        }
                    } else if (!Codec::isEmpty(word)) {
                        a.occupied = true;
                        return false;
                    }
                }

                // ─── §7.1 / DEC-068: draw this row's deferred release ───────
                //
                // The three rows that displace a `Mapping` from a DIRECTLY
                // WRITTEN slot. `DetachChild` is deliberately absent: a fully
                // covered child's displaced values all ride node deleters, which
                // is why §7.1 calls the PARTIAL cover the expensive shape.
                // `ShrinkLeafInPlace` is absent because the slot still names the
                // same record (±0), and `WriteLeaf` because nothing was there.
                //
                // The draw is here rather than in commit because it can fail and
                // commit cannot; the row set is identical because commit acts
                // only on rows this attempt holds a bit for, and every row it
                // could skip was a NoOp or a descend here — neither of which
                // draws.
                Mapping* displaced = nullptr;
                if (d.action == DispatchAction::OverwriteLeaf ||
                    d.action == DispatchAction::ClearSlot) {
                    displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                } else if (d.action == DispatchAction::Subdivide && Codec::isLeaf(word)) {
                    displaced = static_cast<Mapping*>(Codec::decodeLeaf(word));
                }
                if (displaced != nullptr) {
                    switch (deferMappingRelease(displaced, a)) {
                    case DrawResult::Ok:
                        break;
                    case DrawResult::Shortfall:
                        a.recordShortfall = true;
                        return false;
                    case DrawResult::OverBudget:
                        a.recordsOverBudget = true;
                        return false;
                    }
                }
            }
            return true;
        }

        // Every node of the subtree AS IT IS NOW must be one this attempt holds
        // a whole-node claim on. Any extra node means the subtree grew under us
        // between the read pass and the claims: release everything and retry
        // from a fresh read pass (which re-counts, and decomposes if the subtree
        // is now over budget).
        static bool detachmentIsFrozen(NodeRef node, unsigned level, ClaimSet<G, DetachBudget>& set) {
            if (!holdsWholeNode(set, node, level)) return false;
            const unsigned n = Geo<G>::valence(level);
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
        // ─── What this does NOT do, corrected (R-19) ───────────────────────
        //
        // This used to claim it performs §6.1's phase order — "ALL marks, then the
        // §7.2 `+1`s, then all publishes, then retires, then releases" — and cited
        // a rejected formulation for *interleaving* them. **It interleaves them
        // too.** There is one per-slot loop below, and each iteration does its own
        // row's mark, its own `+1`s, its own publish and its own retire before the
        // next slot is looked at. Occupancy is batched to the end of the node, and
        // the record retires happen after the whole walk (`retireHeldRecords`), but
        // nothing here executes five global phases.
        //
        // §6.1's order is **normative about the terminal state and about the
        // boundary**, not about statement order. What it forbids is an irreversible
        // step BEFORE the commit boundary, which `Attempt::phase` checks at every
        // mark, publish and retire. After the boundary nothing can fail, so there
        // is no unwind for an ordering to protect.
        //
        // What safety actually rests on, since it is worth naming rather than
        // inferring from a phase list:
        //
        //   * **Every CLAIMED site is frozen** — and the qualifier is the whole
        //     point, because this note's first version dropped it and the same
        //     function contradicts it seventy lines down (D-070). The claims were
        //     acquired before the boundary and held across the walk, so no other
        //     writer can be in a slot this attempt holds. An UNCLAIMED row is free
        //     to change again between re-dispatch and here — the reachable shape is
        //     a slot the read pass saw empty (a no-op row, which takes no bit) that
        //     a concurrent writer then filled — so commit re-checks and DECLINES
        //     those rows rather than trusting them, counting them in
        //     `unheldRowsSkipped`. That is D-013, and it is a live path, not a
        //     defensive one.
        //   * **The atomicity unit is ONE SLOT WORD**, single-word and
        //     release-stored (kSlotPublish), giving readers old-or-complete-new per
        //     slot. Cross-slot atomicity was never promised to readers, so a reader
        //     legitimately observing slot 3's new value and slot 4's old one is not
        //     a torn state — it is a state the protocol permits.
        //   * **Per-row order is still load-bearing where it is written**, and the
        //     DetachChild row is the case: `markSubtree` before `publishAfterMark`,
        //     so a descent-cache entry into an interior node of the subtree sees the
        //     mark rather than resuming into unlinked structure. That is a local
        //     sequencing requirement, not an instance of the global phase list.
        bool commit(NodeRef node, unsigned level, uint64_t nodeBase,
                    uint64_t lo, uint64_t hi, VMSubstrate::SafePtr<Mapping> value, Attempt& a) {
            const bool     writes = (value != nullptr);
            const uint64_t span   = Geo<G>::slotSpan(level);
            const unsigned first  = slotIndexFor(level, nodeBase, lo);
            const unsigned last   = slotIndexFor(level, nodeBase, hi);

            int occupancyDelta = 0;

            for (unsigned i = first; i <= last; i++) {
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;
                const uint64_t slotEnd  = slotBase + span - 1;
                const uint64_t clipLo   = lo > slotBase ? lo : slotBase;
                const uint64_t clipHi   = hi < slotEnd  ? hi : slotEnd;

                // ACQUIRE, not RELAXED (R-9) — same mixed-claim walk as
                // re-dispatch, and the `DescendIntoChild` row below dereferences
                // the child this word decodes to.
                const uint64_t word = node.slot(i).load(kAttemptSlotLoad);
                const DispatchResult d =
                    dispatchSlot<G, Codec>(word, level, slotBase, clipLo, clipHi, writes);

                // D-013 forcing function. Every row below that STORES to
                // `node.slot(i)` must hold that slot's claim bit. The two
                // exemptions are documented and real: `NoOp` writes nothing, and
                // `DescendIntoChild`'s reclamation store is protected by the
                // CHILD's whole-node claim instead — "the interlock lives in the
                // child, not in the parent slot" (Claim.h).
                // ─── D-013: only a CLAIMED slot is frozen ──────────────────
                //
                // Re-dispatch validated every row, but validation only STAYS true
                // for slots this attempt holds a bit on. An unclaimed slot is
                // free to change again in the window between re-dispatch and
                // here, and commit re-reads the word — so a row can appear that
                // no claim covers.
                //
                // The reachable shape is a clearing operation: the read pass saw
                // the slot EMPTY, which is a no-op row that takes no bit, and a
                // concurrent writer filled it afterwards. Acting on it would
                // clear a slot this attempt never reserved — moving an occupancy
                // count without the interlock (§5.3's assert, the only detector
                // over-counting has) and releasing a `Mapping` reference it never
                // took (a refcount underflow, seen as a use-after-poison on the
                // record). Both symptoms, one cause.
                //
                // Declining is not merely safe, it is the CORRECT answer: the
                // interloping write landed after this operation's claims, so the
                // execution in which the clear happened first and the write
                // second is a legal linearization, and it is the one where the
                // new mapping survives. Retrying instead would also be sound but
                // strictly worse — it re-runs the whole operation to reach the
                // same end state, under contention, forever.
                //
                // A WRITING row here would be a different matter — it would be a
                // LOST WRITE rather than a legal ordering, so it stays an assert.
                // It should be unreachable: a writing operation takes a bit for
                // every slot it touches, including empty ones (an empty slot
                // dispatches to WriteLeaf, not to a no-op), so it never reaches
                // commit with an unclaimed row. The assert is what keeps that
                // reasoning honest if a future dispatch row breaks it.
                if (d.action != DispatchAction::NoOp &&
                    d.action != DispatchAction::DescendIntoChild &&
                    !holdsSlot(a.claims, node, i)) {
                    assert(!writes,
                           "radix: commit reached an unclaimed WRITING row — that is a lost "
                           "write, not a legal reordering (D-013)");
                    counters.unheldRowsSkipped.fetch_add(1, kRefcountAcquire);
                    continue;
                }

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
                        markUnderClaim(child, level + 1, a);
                        retainClaim(a.claims, child);
                        publishUnderChildClaim(node, i, 0, a);
                        occupancyDelta--;
                        retireNode(child, level + 1, a);
                    }
                    break;
                }

                case DispatchAction::WriteLeaf:
                    value->acquireRef();                       // commit-phase +1
                    publish(node, i, Codec::encodeLeaf(value.address(), d.range, level), a);
                    occupancyDelta++;
                    break;

                case DispatchAction::OverwriteLeaf: {
                    // The displaced record's −1 was charged to a DeferredRelease
                    // record at re-dispatch and lands at grace-period end; there
                    // is deliberately nothing to do here. The synchronous form
                    // that used to sit on this line is §10's named
                    // use-after-free: a reader inside the section that observed
                    // the old slot word takes its counted reference after this
                    // CPU has taken the count to zero.
                    value->acquireRef();                       // +1 BEFORE the publish
                    publish(node, i, Codec::encodeLeaf(value.address(), d.range, level), a);
                    break;
                }

                case DispatchAction::ShrinkLeafInPlace:
                    // +-0. Deliberately does not touch the count and deliberately
                    // does not share a path with the rows that do.
                    publish(node, i,
                            Codec::encodeLeaf(Codec::decodeLeaf(word), d.range, level), a);
                    break;

                case DispatchAction::ClearSlot: {
                    // The −1 rides a DeferredRelease record drawn at re-dispatch.
                    publish(node, i, 0, a);
                    occupancyDelta--;
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
                        replacement = Codec::encodeLeaf(value.address(), sub, level);
                    }
                    // Phase two of §6.5: mark the WHOLE subtree, once it is held
                    // exclusively and nothing can fail. Recursive marking alone
                    // would reproduce the irreversibility trap at scale — a
                    // marker that marks half a subtree and then fails deep has
                    // poisoned every node it touched.
                    markSubtree(child, level + 1, a);
                    // The parent-slot store happens AFTER the marking phase, and
                    // is a publish under a bit claimed earlier — not an
                    // acquisition after a mark. A naive "mark is last" assert
                    // must exempt it explicitly.
                    publishAfterMark(node, i, replacement, a);
                    if (!writes) occupancyDelta--;
                    retireSubtree(child, level + 1, a);
                    break;
                }

                case DispatchAction::Subdivide: {
                    void* child = a.takePending(nodeBase, level, i, word);
                    assert(child, "radix: commit reached a subdivision with no prebuilt "
                                  "subtree — re-dispatch should have caught this before the "
                                  "boundary");
                    // Take every `+1` the new subtree's leaf slots imply, THEN
                    // publish. The displaced record's −1 rides a DeferredRelease
                    // record drawn at re-dispatch, so the "increments must
                    // precede the decrement" hazard — a subdivision whose
                    // survivors name the record it is displacing taking the count
                    // transiently to zero — is now structural rather than a
                    // matter of statement order: the decrement cannot land until
                    // a grace period after these increments.
                    //
                    // Note the edge-subdivision shape is net zero on the old
                    // record and STILL draws a record (§7.1): the +1s and the
                    // deferred −1 are separate commit steps, not a cancellation.
                    takeSubtreeReferences(NodeRef(child), level + 1);
                    publish(node, i, Codec::encodeChild(child), a);
                    if (Codec::isEmpty(word)) occupancyDelta++;
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
            if (node == a.binding.root) return false;
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
        // Runs AFTER the section has closed, so it may not touch a node: every
        // claim must already have been released inside the section that observed
        // the links (§3.2/§7.3). Discarding never-published allocations is safe
        // there — they are reachable by nothing and outside the protocol.
        void abandon(Attempt& a) {
            assertNoClaimsHeld(a, "abandon");
            // Records go home inside the section, beside the claim release, so
            // by here the attempt must hold none. Asserting rather than sweeping
            // is deliberate: a sweep would silently paper over a return path that
            // forgot, and the symptom of a leaked record is not a crash but a
            // pool that runs a little drier after every operation.
            assert(a.heldReleases == nullptr && a.heldReleaseCount == 0,
                   "radix: a DeferredRelease record survived to `abandon` — records are "
                   "returned to their home pool inside the section (§7.3), so one still "
                   "held here has escaped the protocol");
            discardUnusedAllocations(a);
        }

        // §11's other half of the progress row: "a debug assert that no claim is
        // held across an allocation or a SECTION CLOSE."
        static void assertNoClaimsHeld(const Attempt& a, const char* where) {
            for (size_t i = 0; i < a.claims.count; i++) {
                assert(!a.claims.entries[i].held,
                       "radix: a claim is still held at a point where none may be — a claim "
                       "outliving its section makes the claim set a set of pointers loaded "
                       "in a section that has closed (§3.2/§7.3)");
            }
            (void)where;
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
            assert(a.phase == Attempt::Phase::Committing,
                   "radix: a node retired before the commit boundary (§11)");
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
            const unsigned n = Geo<G>::valence(level);
            assert(claimsFullValence(a.claims, node, level),
                   "radix: retireSubtree reading slots with kClaimedSlotLoad (RELAXED) on a "
                   "node this attempt does not hold the full valence mask for — the "
                   "constant's ordering rests on that claim (R-9)");
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
        static void markSubtree(NodeRef node, unsigned level, Attempt& a) {
            markUnderClaim(node, level, a);
            retainClaim(a.claims, node);
            const unsigned n = Geo<G>::valence(level);
            // `markUnderClaim` above asserted `holdsWholeNode`, but `retainClaim`
            // has since cleared `held`, so the re-check below is the mask-only form
            // — the physical claim the RELAXED load rests on.
            assert(claimsFullValence(a.claims, node, level),
                   "radix: markSubtree reading slots with kClaimedSlotLoad (RELAXED) "
                   "without the full valence mask (R-9)");
            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(kClaimedSlotLoad);
                if (Codec::isChild(w)) {
                    markSubtree(NodeRef(Codec::decodeChild(w)), level + 1, a);
                }
            }
        }

        template <typename F>
        static void walkNode(NodeRef node, unsigned level, uint64_t nodeBase, F& fn) {
            const unsigned n = Geo<G>::valence(level);
            const uint64_t span = Geo<G>::slotSpan(level);
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
            const unsigned v = Geo<G>::valence(level);
            for (unsigned i = 0; i < v; i++) {
                const uint64_t w = node.slot(i).load(kQuiescedRead);
                if (Codec::isChild(w)) countNodes(NodeRef(Codec::decodeChild(w)), level + 1, n);
            }
        }
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_CORE_TREE_H
