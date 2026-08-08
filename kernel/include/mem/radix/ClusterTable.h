//
// radix-tree — the cluster table: creation and growth (§5.6; DEC-026 / 027 /
// 028 / 067 / 090 / 103).
//
// The table is the one page of buckets plus the two operations that write it.
// Everything else about a cluster is an ordinary core-tree operation against
// whatever the bucket word currently decodes to.
//
// ─── One publish, one word, one CAS ───────────────────────────────────────
//
// Growth changes the root pointer, the root's LEVEL and the cluster's BASE
// together, and DEC-103 packs all three into one word for exactly that reason:
// the packing is what keeps growth the same single-publish shape as everything
// else in the tree. **Every root-bucket write is a compare-exchange, and this is
// the one publish site DEC-055's release-store rule does not cover** — a bucket
// has no state word and therefore no claim bit to make its writer exclusive, so
// the CAS *is* the arbitration.
//
// ─── The accounting, which is the part to get right ───────────────────────
//
// §5.6 is emphatic and the phase plan repeats it: **no count moves on the old
// root, nothing is retired, nothing reverts.** Spelled out, because each clause
// has a natural-looking wrong version:
//
//   - A node published by a bucket CAS — growth's new parent, creation's empty
//     root — is **constructed with its structural count pre-assigned to 1**.
//     That is not a `+1` on a published object (the node is private until the
//     CAS wins); it is the initial condition publication requires. A root
//     observable at count 0 would let a descent cache's paired
//     increment/release take it 0 → 1 → 0 and destroy a live node.
//   - **The old root's count never moves.** The bucket word's inbound reference
//     transfers *in place* to the new parent's child slot. The grower takes no
//     `+1`, so a loser has nothing to revert — which is what deleted DEC-067's
//     one exception and made §7.2's commit-phase rule exceptionless.
//   - **Nothing is retired.** The old bucket word is a value, not an object, and
//     the old root stays reachable as the new root's child.
//   - A losing grower or creator **shallow-discards** its private node —
//     a direct destroy of a never-published allocation, *its pre-assigned count
//     notwithstanding* (§7.3). The count was the initial condition for a
//     publication that never happened, not an owned reference.
//
// ─── Why creation publishes an EMPTY root ─────────────────────────────────
//
// It costs a second publish — creation is the one place the fused DEC-007
// verify-is-install pass does not hold — and count-cleanness is what that buys.
// A pre-populated private root would carry `Mapping` `+1`s through a losable
// CAS with no safe revert: the loser cannot release them (the record may have
// reached zero on the winner's path) and cannot keep them (they name slots that
// were never published). Publishing empty and placing after makes the loser's
// discard trivially count-free, and creation is rare.
//
// The same hazard is why creation needs the CAS at all: two threads probing into
// the same empty bucket would otherwise both mint a cluster and both store,
// losing one and every mapping placed in it.
//

#ifndef CROCOS_RADIX_CLUSTER_TABLE_H
#define CROCOS_RADIX_CLUSTER_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <mem/VMSubstrate.h>
#include <rcu/RCU.h>

#include <mem/radix/BucketCodec.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/Geometry.h>
#include <mem/radix/Node.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    // The smallest level whose NODE SPAN covers `bytes` (DEC-090). Necessity,
    // not adaptivity: a probed placement whose request plus its guard gaps
    // exceeds the default cluster span must still fit inside one cluster, and
    // DEC-080 bounds it above at the bucket. Returns the default root level for
    // anything that fits it — a smaller request never gets a *deeper* root,
    // because depth is what the default level is chosen for.
    template <GeometryDescriptor G>
    constexpr unsigned rootLevelCovering(uint64_t bytes) {
        unsigned level = G.defaultRootLevel;
        while (level > 1 && nodeSpan(G, level) < bytes) level--;
        return level;
    }

    enum class ClusterStatus : uint8_t {
        Ok,
        // vmsmalloc DEC-048 / DEC-075: `tryMake` returned null. Surfaced as
        // ENOMEM; never a panic, because untrusted userspace reaches this path.
        OutOfMemory,
        // The bucket already holds a cluster. Not a failure — a losing creator
        // adopts the winner's cluster (§5.6), and this is how it finds out.
        AlreadyPresent,
    };

    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    class ClusterTable {
    public:
        using Tree         = CoreTree<G, Codec, DetachBudget>;
        using BucketCodecT = BucketCodec<G, typename Codec::BasePolicy>;
        using Ops          = NodeOps<G>;

        ClusterTable() = default;
        ClusterTable(const ClusterTable&)            = delete;
        ClusterTable& operator=(const ClusterTable&) = delete;

        // The table is one small page and is allocated through the failable
        // path like everything else — it is reached from address-space creation,
        // which untrusted userspace drives.
        [[nodiscard]] bool init(kernel::rcu::Domain& d, DeferredReleasePools& pools) {
            assert(d.initialized(), "radix: the cluster table's domain must be initialised");
            auto p = VMSubstrate::tryMake<BucketTable>();
            if (!p) return false;
            table   = static_cast<BucketTable*>(p.raw());
            domain  = &d;
            release = &pools;
            return true;
        }

        // §7.4's WALK, split from the page free because the sequence separates
        // them: the walk is step 4 and the root page goes at step 6, after
        // `drainAllQuiescent`. Conflating them frees the page while retired
        // nodes are still in bags naming slots inside it.
        //
        // The walk itself is not yet DEC-100's unit-decomposed one — see D-032.
        void tearDownClusters() {
            if (table == nullptr) return;
            for (size_t i = 0; i < kBucketCount; i++) {
                const uint64_t w = table->entries[i].load(kQuiescedRead);
                if (BucketCodecT::isEmpty(w)) continue;
                Tree t = treeFor(i);
                t.destroyTree();
            }
        }

        void freeRootPage() {
            if (table == nullptr) return;
            VMSubstrate::destroy(VMSubstrate::SafePtr<BucketTable>(table));
            table = nullptr;
        }

        // The two together, for callers outside the §7.4 sequence (the growth
        // tests, which have no address space around them).
        void destroy() {
            tearDownClusters();
            freeRootPage();
        }

        [[nodiscard]] bool valid() const { return table != nullptr; }
        [[nodiscard]] BucketTable& buckets() const { return *table; }

        // A core-tree view of one bucket. A VALUE, deliberately: it holds no
        // decoded root, only the bucket it must read, so every operation on it
        // re-decodes inside its own section. Constructing one is free.
        [[nodiscard]] Tree treeFor(size_t bucketIndex) const {
            assert(table != nullptr, "radix: cluster table used before init");
            assert(bucketIndex < kBucketCount, "radix: bucket index out of range");
            Tree t;
            t.bindToBucket(*table, bucketIndex, *domain, *release);
            return t;
        }

        [[nodiscard]] bool bucketIsOccupied(size_t bucketIndex) const {
            return !BucketCodecT::isEmpty(table->entries[bucketIndex].load(kQuiescedRead));
        }

        // ─── Creation (§5.6) ───────────────────────────────────────────────
        //
        // Publishes an EMPTY root rooted at `rootLevel`, covering the
        // span-aligned cluster containing `va`. The caller places into it
        // afterwards with an ordinary claimed-slot operation.
        //
        // `AlreadyPresent` is not an error: a losing creator adopts the winner's
        // cluster, which is exactly what the caller does with this result.
        [[nodiscard]] ClusterStatus createCluster(uint64_t va, unsigned rootLevel) {
            assert(table != nullptr, "radix: cluster table used before init");
            assert(rootLevel >= 1 && rootLevel <= G.levelCount,
                   "radix: cluster root level out of range");

            const size_t   idx  = bucketIndexFor<G>(va);
            const uint64_t base = roundDownTo(va, nodeSpan(G, rootLevel));
            assert(bucketIndexFor<G>(base) == idx,
                   "radix: a cluster rooted this high would start outside its own bucket — "
                   "DEC-033 gives one cluster per bucket and the index is a prefix");

            // Cheap pre-check outside the CAS. Purely an optimisation: the CAS
            // below is what actually arbitrates, and a bucket that fills between
            // here and there simply loses.
            if (bucketIsOccupied(idx)) return ClusterStatus::AlreadyPresent;

            // Pre-assigned count of 1 — the reference the winning CAS's bucket
            // word will hold. See the header comment for why this is an initial
            // condition rather than a `+1`.
            void* root = Ops::alloc(rootLevel, /*initialRefcount=*/1);
            if (root == nullptr) return ClusterStatus::OutOfMemory;

            const uint64_t word = BucketCodecT::encode(root, rootLevel, base, idx);
            uint64_t expected = 0;
            if (table->entries[idx].compare_exchange(
                    expected, word, kBucketPublishSuccess, kBucketPublishFailure)) {
                return ClusterStatus::Ok;
            }

            // Lost. Shallow-discard: a direct destroy of a never-published
            // allocation, pre-assigned count notwithstanding. Nothing to revert,
            // because nothing was ever taken.
            Ops::destroy(rootLevel, NodeRef(root));
            return ClusterStatus::AlreadyPresent;
        }

        // ─── Growth (§5.6) ─────────────────────────────────────────────────
        //
        // Grows the cluster in `va`'s bucket until its span covers [lo, hi].
        // A fixed-address request into an occupied bucket grows that cluster
        // until its span covers the target rather than allocating a second
        // cluster in the bucket (DEC-033/§5.6).
        //
        // Loops because a lost CAS may or may not have done our work for us: the
        // winner's growth may already cover what we need, in which case we are
        // done, and if it does not we retry from its word. Growth cannot
        // collide, since one cluster per bucket means a widened span is by
        // construction still inside its bucket.
        [[nodiscard]] ClusterStatus growToCover(uint64_t lo, uint64_t hi) {
            assert(table != nullptr, "radix: cluster table used before init");
            assert(lo <= hi, "radix: empty range");
            const size_t idx = bucketIndexFor<G>(lo);
            assert(bucketIndexFor<G>(hi) == idx,
                   "radix: growToCover over a bucket-crossing range — DEC-080 decomposes "
                   "those in the VMM, before the tree is involved");

            for (;;) {
                typename BucketCodecT::Decoded cur;
                {
                    // §3.1/§7.3: one protected load, all three fields, nothing
                    // held past the close. The old root pointer is used only
                    // inside this section — to build the parent's child slot —
                    // and the CAS that publishes it happens here too.
                    kernel::rcu::ReadGuard guard(*domain);
                    const uint64_t word =
                        kernel::rcu::protectWord(*domain, table->entries[idx]);
                    if (BucketCodecT::isEmpty(word)) return ClusterStatus::AlreadyPresent;
                    cur = BucketCodecT::decode(word, idx);

                    if (covers(cur, lo, hi)) return ClusterStatus::Ok;

                    assert(cur.level > 1,
                           "radix: a cluster already rooted at the top of its zone cannot "
                           "grow further, so a range it does not cover must have crossed a "
                           "bucket boundary (DEC-080)");

                    const unsigned newLevel = cur.level - 1;
                    const uint64_t newBase  = roundDownTo(cur.base, nodeSpan(G, newLevel));

                    void* parent = Ops::alloc(newLevel, /*initialRefcount=*/1);
                    if (parent == nullptr) return ClusterStatus::OutOfMemory;

                    // Wire the old root into the parent's child slot. The
                    // bucket's inbound reference TRANSFERS IN PLACE — no count
                    // moves, on either node. `kPrivateInit` because the parent is
                    // unreachable until the CAS, whose release ordering covers
                    // everything written here.
                    const unsigned slot = static_cast<unsigned>(
                        (cur.base - newBase) / slotSpan(G, newLevel));
                    assert(slot < valence(G, newLevel), "radix: growth slot out of range");
                    NodeRef p(parent);
                    p.slot(slot).store(Codec::encodeChild(cur.root), kPrivateInit);
                    // One occupied slot. Set directly rather than through
                    // bumpOccupancy's RMW: the node is private, so there is
                    // nothing to be atomic against, and the CAS publishes it.
                    p.stateWord().store(state::kCountUnit, kPrivateInit);

                    const uint64_t newWord =
                        BucketCodecT::encode(parent, newLevel, newBase, idx);
                    uint64_t expected = word;
                    if (table->entries[idx].compare_exchange(
                            expected, newWord, kBucketPublishSuccess,
                            kBucketPublishFailure)) {
                        continue;   // re-read: one growth may not be enough
                    }

                    // Lost. Shallow-discard the private parent — and note the
                    // discard must NOT walk into its child slot, which names the
                    // still-live old root. `Ops::destroy` is the single-node
                    // form for exactly this reason; the recursive
                    // destroyUnpublishedSubtree would take the whole cluster
                    // with it.
                    Ops::destroy(newLevel, p);
                }
                // Re-read from the winner's word on the next iteration.
            }
        }

        // Grow-or-create: the shape a fixed-address placement wants. Creates the
        // cluster if the bucket is empty, then grows it until it covers.
        [[nodiscard]] ClusterStatus ensureCovers(uint64_t lo, uint64_t hi) {
            const size_t idx = bucketIndexFor<G>(lo);
            if (!bucketIsOccupied(idx)) {
                const uint64_t bytes = hi - lo + 1;
                const unsigned level = rootLevelCovering<G>(bytes);
                const ClusterStatus st = createCluster(lo, level);
                if (st == ClusterStatus::OutOfMemory) return st;
                // Ok or AlreadyPresent both mean "a cluster is there now"; the
                // growth below reconciles whichever one it is with our range.
            }
            return growToCover(lo, hi);
        }

    private:
        static constexpr uint64_t roundDownTo(uint64_t v, uint64_t align) {
            return v & ~(align - 1);
        }

        static bool covers(const typename BucketCodecT::Decoded& d,
                           uint64_t lo, uint64_t hi) {
            const uint64_t end = d.base + nodeSpan(G, d.level) - 1;
            return lo >= d.base && hi <= end;
        }

        BucketTable*          table   = nullptr;
        kernel::rcu::Domain*  domain  = nullptr;
        DeferredReleasePools* release = nullptr;
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_CLUSTER_TABLE_H
