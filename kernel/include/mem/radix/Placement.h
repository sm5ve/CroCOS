//
// radix-tree — placement (§5.6; DEC-007 / 008 / 013 / 032 / 091 / 095).
//
// ─── Probe, then scan, then say so ────────────────────────────────────────
//
// "**K random probes (provisionally 8), then one bounded free-run scan, then a
// distinct `kNoSpaceInCluster` result**" (DEC-095). The three-stage shape is a
// correction, not a design flourish: the original O(1)-probe claim was computed
// against per-ADDRESS-SPACE occupancy, and probing actually runs against
// per-CLUSTER occupancy. A process with a few hundred MiB in a 1 GiB cluster
// probes at tens of percent, so the scan is a priced slow path rather than a
// rare fallback, and out-of-space is an explicit mechanism result the policy
// answers by growing or re-pointing — never a fallback the mechanism performs
// on its own.
//
// **No gap augmentation** (DEC-007). Storing a largest-free-gap per subtree —
// Linux's `rb_subtree_gap` — would force every `mmap` and `munmap` to propagate
// to the root, reintroducing exactly the global contention this design exists to
// remove, to solve a problem sparsity means we do not have.
//
// ─── Why the install is fused, and where the fusion stops ─────────────────
//
// The verify pass IS the install pass (DEC-007), and that is a correctness
// property before it is a performance one: verify-then-install in two traversals
// lets a concurrent placement take the range in between, and the second install
// silently overwrites it. `ApplyMode::OnlyIfEmpty` moves the check
// inside the attempt, where the claims freeze every slot it will write.
//
// The **guard gaps** are deliberately outside that fusion, and this is the one
// place the mechanism is best-effort by design. A gap is space we decline to
// use, not space we own: checking it under the same claims would mean claiming
// slots the mapping does not occupy, which turns two placements that merely
// want breathing room into two placements that conflict. So the gap is verified
// unfused, a concurrent placement may take part of it, and the cost is the gap
// — never a lost mapping. §5.6's own framing supports this: "in a sparse space
// they cost nothing", i.e. they are an entropy and overflow-protection
// nicety, not an invariant.
//
// ─── Entropy ─────────────────────────────────────────────────────────────
//
// Probing and gap sizing consume randomness. Placement takes its entropy as a
// PARAMETER — the harness passes Phase 0's seedable source, the kernel passes
// `kernel::random::Entropy` — and that is a seam rather than a stub: the quality
// requirement differs per consumer (ASLR placement wants a CSPRNG; the backoff
// jitter in CoreTree deliberately does not), so a single hard-wired source would
// have to satisfy the stricter one everywhere.
//
// **The kernel source is a placeholder and is not a CSPRNG** (DEC-063's real
// work is still owed). `kernel/include/Random.h` carries the warning and the
// checklist for replacing it. What matters here is that placement does not
// have to change when that happens.
//

#ifndef CROCOS_RADIX_PLACEMENT_H
#define CROCOS_RADIX_PLACEMENT_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>

#include <core/utility.h>   // the AssignmentPolicy concept's requires-clause
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/Geometry.h>

namespace kernel::mm::radix {

    // DEC-013: **placement granularity and the resolution floor are separate
    // numbers.** Placement is policy — 64 KiB; the floor is capability — 4 KiB.
    // Windows has shipped exactly this split since NT. The floor cannot itself
    // be 64 KiB because `mprotect` is page-granular in POSIX, which would make a
    // sub-granularity split unrepresentable.
    inline constexpr uint64_t kPlacementGranularity = uint64_t{64} << 10;

    // DEC-095, provisional: how many random probes before the scan.
    inline constexpr unsigned kProbeCount = 8;

    // Guard gaps, in placement granules. Random-sized (DEC-008); the cap is a
    // tuning figure, not a correctness one.
    inline constexpr unsigned kMaxGuardGranules = 4;

    enum class PlaceStatus : uint8_t {
        Ok,
        // DEC-095's explicit mechanism result, defined as **"no free run
        // observed by this scan"**. The policy answers it by growing the cluster
        // or re-pointing to another one (DEC-091); the mechanism never does
        // either on its own.
        NoSpaceInCluster,
        OutOfMemory,
    };

    struct PlacementResult {
        PlaceStatus status = PlaceStatus::NoSpaceInCluster;
        uint64_t    va     = 0;
    };

    // Round a byte count up to whole placement granules.
    constexpr uint64_t placementSpanFor(uint64_t bytes) {
        return (bytes + kPlacementGranularity - 1) & ~(kPlacementGranularity - 1);
    }

    // The unfused half. Best-effort by design — see the header comment for why
    // claiming the gap would turn breathing room into contention.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget>
    [[nodiscard]] bool guardIsClear(const CoreTree<G, Codec, DetachBudget>& tree,
                                    uint64_t va, uint64_t span, uint64_t guard,
                                    uint64_t clusterLo, uint64_t clusterHi) {
        if (guard == 0) return true;
        const uint64_t lo = (va > clusterLo + guard) ? va - guard : clusterLo;
        const uint64_t hi = (va + span - 1 + guard < clusterHi) ? va + span - 1 + guard
                                                                : clusterHi;
        bool clear = true;
        // The record is never named, let alone read — occupancy is the whole
        // question — so this callback is the one shape that owes nothing.
        tree.enumerate(lo, hi,
                       [&](VMSubstrate::SafePtr<Mapping>, uint64_t, uint64_t) { clear = false; });
        return clear;
    }

    // ─── The mechanism (DEC-032) ───────────────────────────────────────────
    //
    // **The base placement API takes an explicit cluster**, splitting mechanism
    // from policy: this places within a GIVEN cluster, and *which* cluster is a
    // policy layered above it (DEC-091). The call is internal to the VMM from
    // the outset and never reachable from an `mmap` caller — an unexported
    // primitive is a seam rather than a footgun.
    //
    // It is also what makes §11's adjacency target directly drivable, instead of
    // dependent on an assignment policy happening to co-locate two allocators.
    //
    // `entropy()` must return a uniformly distributed uint64_t.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget, typename Entropy>
    [[nodiscard]] PlacementResult placeInCluster(
            CoreTree<G, Codec, DetachBudget>& tree,
            uint64_t bytes, VMSubstrate::SafePtr<Mapping> value, Entropy&& entropy) {
        assert(value != nullptr, "radix placement: a placement with no record to place");
        assert(bytes > 0, "radix placement: zero-length placement");

        const uint64_t span = placementSpanFor(bytes);
        const uint64_t base = tree.base();
        const uint64_t size = tree.span();
        if (span > size) return {PlaceStatus::NoSpaceInCluster, 0};

        // The number of aligned positions the cluster offers for a span this
        // size. Computed rather than assumed non-zero: an oversized request into
        // a small cluster has exactly one, and a probe that took `% 0` would be
        // a divide fault on the placement path.
        const uint64_t positions = (size - span) / kPlacementGranularity + 1;

        // ─── Stage 1: K random probes, fused verify-is-install ─────────────
        for (unsigned k = 0; k < kProbeCount; k++) {
            const uint64_t guard =
                (entropy() % (kMaxGuardGranules + 1)) * kPlacementGranularity;
            const uint64_t va = base + (entropy() % positions) * kPlacementGranularity;

            if (!guardIsClear(tree, va, span, guard, base, base + size - 1)) continue;

            const ApplyStatus st =
                tree.apply(va, va + span - 1, value, ApplyMode::OnlyIfEmpty);
            if (st == ApplyStatus::Ok)          return {PlaceStatus::Ok, va};
            if (st == ApplyStatus::OutOfMemory) return {PlaceStatus::OutOfMemory, 0};
            // ApplyStatus::Occupied — an ordinary miss. Probe again.
        }

        // ─── Stage 2: the bounded, chunked free-run scan ───────────────────
        //
        // The candidate is not a reservation, so a stale one just resumes the
        // scan. The cursor is monotonic, which is what makes that terminate
        // rather than hand out the same VA forever.
        // Loop on the CURSOR, not on the chunk's return value. A chunk returns
        // false for two different reasons — "no run in this chunk's budget, call
        // again" and "the scan reached the end" — and only the cursor
        // distinguishes them. Conflating them stops the scan 64 slots in, which
        // presents as a cluster reporting itself full while most of it is empty:
        // a placement failure, not a crash, and one that looks exactly like
        // genuine exhaustion.
        auto cursor = tree.freeRunScanFrom(base, base + size - 1);
        while (!cursor.finished()) {
            uint64_t candidate = 0;
            if (!tree.findFreeRunChunk(cursor, span, kPlacementGranularity, candidate)) {
                continue;   // budget spent; the cursor advanced
            }
            const ApplyStatus st =
                tree.apply(candidate, candidate + span - 1, value, ApplyMode::OnlyIfEmpty);
            if (st == ApplyStatus::Ok)          return {PlaceStatus::Ok, candidate};
            if (st == ApplyStatus::OutOfMemory) return {PlaceStatus::OutOfMemory, 0};
        }

        // ─── Stage 3 ───────────────────────────────────────────────────────
        return {PlaceStatus::NoSpaceInCluster, 0};
    }


    // ─── The cluster-assignment seam (DEC-091) ─────────────────────────────
    //
    // Which cluster a probed placement goes to is POLICY, and Spencer settled it
    // as "behind a swappable policy seam" rather than as a fixed rule. So this
    // is a concept, not a class: anything with `bucketFor` and `noteOutOfSpace`
    // serves, and the harness ships a second, deliberately silly one as the
    // cheap proof the seam is real. A seam with one implementation is an
    // assertion, not a seam.
    //
    // The one thing the seam does NOT get to vary: **the per-CPU cell holds a
    // BUCKET INDEX, never a decoded pointer.** A decoded root is uncounted and
    // dies at a section close (§7.3), so caching one across operations is the
    // same defect as §3.1's rejected node-pointer token — the index is stable
    // precisely because it is not a pointer.
    template <typename P>
    concept AssignmentPolicy = requires(P p, arch::ProcessorID cpu, size_t idx,
                                        uint64_t entropy) {
        { p.bucketFor(cpu, entropy) } -> convertible_to<size_t>;
        { p.noteOutOfSpace(cpu, idx) };
    };

    // The default: one cluster per CPU, so concurrent allocators work in
    // disjoint subtrees — "the same shape as vmsmalloc's per-CPU arenas"
    // (DEC-008). First use picks a bucket from entropy; out-of-space re-points
    // that CPU at a fresh one rather than growing, which is what keeps the
    // partitioning from collapsing as a cluster fills.
    struct PerCpuAssignment {
        static constexpr size_t kNoBucket = ~size_t{0};

        // A cell per CPU. `Atomic` because a re-point can be observed by a
        // concurrent reader of the same cell under a future scheduler, not
        // because two CPUs share one — they do not.
        Atomic<uint64_t> cell[arch::MAX_PROCESSOR_COUNT];

        PerCpuAssignment() {
            for (size_t i = 0; i < arch::MAX_PROCESSOR_COUNT; i++) {
                cell[i].store(kNoBucket, kPrivateInit);
            }
        }

        [[nodiscard]] size_t bucketFor(arch::ProcessorID cpu, uint64_t entropy) {
            const size_t i = static_cast<size_t>(cpu);
            const uint64_t cur = cell[i].load(kQuiescedRead);
            if (cur != kNoBucket) return static_cast<size_t>(cur);
            const size_t chosen = static_cast<size_t>(entropy % kBucketCount);
            cell[i].store(chosen, kPrivateInit);
            return chosen;
        }

        void noteOutOfSpace(arch::ProcessorID cpu, size_t idx) {
            const size_t i = static_cast<size_t>(cpu);
            // Only clear if it is still the bucket we were told about: two
            // placements failing concurrently on the same CPU must not have the
            // second undo the first's re-point.
            uint64_t expected = static_cast<uint64_t>(idx);
            (void)cell[i].compare_exchange(expected, kNoBucket,
                                           kPrivateInit, kQuiescedRead);
        }
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_PLACEMENT_H
