//
// radix-tree — the root bucket table and its packed entry codec
// (§5.1, §5.6; DEC-030 / 033 / 102 / 103).
//
// The root is ONE SMALL PAGE of buckets, and both halves of that sentence are
// derived rather than chosen. DEC-102 makes the count
// `arch::smallPageSize / sizeof(BucketEntry)`, so a 16 KiB-page architecture
// gets 2048 buckets over 64 GiB zones with no edit; DEC-103 pins the entry at
// **one 64-bit word**, which is what lets cluster growth stay a single publish.
//
// ─── What is packed, and why all of it must be ────────────────────────────
//
// Growth changes the root pointer, the root's LEVEL and the cluster's BASE
// together — a cluster grows by allocating a node above its root, so its span
// widens downward in VA and its base can move. Publishing those three
// separately is not a shape the protocol has: a bucket carries no state word, so
// there is no claim bit to make its writer exclusive, and the CAS *is* the
// arbitration (§5.6). Packing them into one word is what keeps growth the same
// single-publish shape as every other write in the tree. The earlier design
// bought the same property with an out-of-line descriptor object, which DEC-103
// deleted as an object class.
//
//     bit 0                     guard — set on every populated entry
//     bits [1, 1+L)             root level, 1..levelCount
//     bits [1+L, 1+L+O)         base offset within the bucket's ZONE, in units
//                               of minClusterSpan
//     bits [1+L+O, 1+L+O+P)     root node pointer, window-relative, shifted
//                               down by the node alignment
//
// The **guard bit** is the DEC-081 move, one section over: a cluster rooted at
// window offset 0, at level 0, base 0, would otherwise encode to all-zero and
// read as an EMPTY BUCKET — a whole zone silently unmappable. All-zero means
// empty and nothing else, structurally.
//
// ─── Field widths are derived, and the cycle is only consistent because the
//     entry width is a constant ────────────────────────────────────────────
//
// DEC-102 derives the bucket count from the entry width; the zone size derives
// from the bucket count; the offset width derives from the zone size. §5.1 is
// explicit about what that means for maintenance: **a failed `static_assert`
// here is resolved by re-cutting fields, never by widening the entry.**
//
// The pointer width derives from the VMSubstrate window over the node
// alignment, and the alignment is computed from the GEOMETRY rather than
// written as 64. §5.1 quotes "the contractual 64 B node alignment", which is
// true of every node type the amd64 geometry produces — but a geometry with a
// 3-bit level has valence-8 nodes at exactly 96 B, and vmsmalloc's 96 B class
// promises 16 B, not 64. Hard-coding 64 would be a codec that special-cases the
// shipped numbers, which §5.1's own DEC-093 constraint forbids. Derived, the
// amd64 instance still lands on 64 B and 45 of 64 bits, exactly as tabulated.
//

#ifndef CROCOS_RADIX_BUCKET_CODEC_H
#define CROCOS_RADIX_BUCKET_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <core/atomic.h>
#include <mem/VMSubstrate.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/Node.h>
#include <mem/radix/Ordering.h>
#include <mem/radix/SlotCodec.h>   // HarnessSlotBase, kSlotPointerAlignment

namespace kernel::mm::radix {

    // Bit 0. Named rather than spelled inline for the same reason
    // kChildGuardBit is: it is the only thing standing between "all-zero means
    // empty" and a zone that silently cannot be mapped.
    inline constexpr uint64_t kBucketGuardBit = uint64_t{1} << 0;

    // ─── The alignment the pointer field may shift by ──────────────────────
    //
    // The contractual slot alignment vmsmalloc promises for a node of this
    // valence. `sizeClassFor` returns the kNumSizeClasses sentinel for the
    // whole-page bypass, whose page alignment dominates any class's.
    constexpr size_t nodeClassAlignment(size_t bytes) {
        const size_t c = vmsmalloc::sizeClassFor(bytes);
        return (c < vmsmalloc::kNumSizeClasses) ? vmsmalloc::slotAlignment(c)
                                                : arch::smallPageSize;
    }

    template <GeometryDescriptor G>
    constexpr size_t nodeAlignmentForValence(unsigned v) {
        switch (v) {
            case 2:  return nodeClassAlignment(sizeof(Node<G, 2>));
            case 4:  return nodeClassAlignment(sizeof(Node<G, 4>));
            case 8:  return nodeClassAlignment(sizeof(Node<G, 8>));
            case 16: return nodeClassAlignment(sizeof(Node<G, 16>));
            case 32: return nodeClassAlignment(sizeof(Node<G, 32>));
            default: return kSlotPointerAlignment;   // DEC-039 caps valence at 32
        }
    }

    // The weakest alignment any CLUSTER ROOT of this geometry can have. Every
    // level, not just the default root level: DEC-090 roots an oversized cluster
    // higher and growth walks it up, so any level 1..levelCount can be a root.
    template <GeometryDescriptor G>
    constexpr size_t clusterRootAlignment() {
        size_t a = arch::smallPageSize;
        for (unsigned l = 1; l <= G.levelCount; l++) {
            const size_t x = nodeAlignmentForValence<G>(valence(G, l));
            if (x < a) a = x;
        }
        return a;
    }

    // ─── The codec ─────────────────────────────────────────────────────────
    //
    // Two instances, exactly like the slot codec and for the same reason: the
    // pointer-compression assumptions are false under test, where the mock arena
    // lives wherever mmap put it. They share the base policy, since both
    // compress against the same window.
    template <GeometryDescriptor G, typename BasePolicy>
    struct BucketCodec {
        static constexpr unsigned kWindowBits = VMSubstrate::kWindowAddressBits;

        static constexpr size_t   kPointerAlignment = clusterRootAlignment<G>();
        static constexpr unsigned kPointerShift     = log2Exact(kPointerAlignment);
        static constexpr unsigned kPointerBits      = kWindowBits - kPointerShift;

        // Levels run 1..levelCount, so the field must hold `levelCount`.
        static constexpr unsigned kLevelBits = log2Exact(G.levelCount + 1);

        // §5.1: "in units of minClusterSpan". A cluster base is span-aligned and
        // spans only grow, so the unit is exact for every representable cluster,
        // grown or large-rooted alike.
        static constexpr uint64_t kZoneSpan        = zoneSpan(G);
        static constexpr uint64_t kMinClusterSpan  = minClusterSpan(G);
        static constexpr unsigned kOffsetBits      =
            log2Exact(kZoneSpan / kMinClusterSpan);

        static constexpr unsigned kLevelShift   = 1;
        static constexpr unsigned kOffsetShift  = kLevelShift + kLevelBits;
        static constexpr unsigned kPointerFieldShift = kOffsetShift + kOffsetBits;

        // The fits-in-64 gate §5.1 asks for. Resolve a failure by re-cutting
        // fields — NEVER by widening the entry, which would change the bucket
        // count, which would change the zone size, which would change this.
        static_assert(kPointerFieldShift + kPointerBits <= 64,
                      "the packed bucket entry does not fit 64 bits — re-cut the fields "
                      "(DEC-102 derives the bucket count from the 8 B entry width, so "
                      "widening the entry changes the zone size and this bound with it)");

        static constexpr uint64_t kLevelMask   = (uint64_t{1} << kLevelBits) - 1;
        static constexpr uint64_t kOffsetMask  = (uint64_t{1} << kOffsetBits) - 1;
        static constexpr uint64_t kPointerMask = (uint64_t{1} << kPointerBits) - 1;

        // ─── Classification ────────────────────────────────────────────────

        static constexpr bool isEmpty(uint64_t word) { return word == 0; }

        // ─── Decoded form ──────────────────────────────────────────────────
        //
        // §7.3: these values are UNCOUNTED and travel with the decoded pointer,
        // so none of them survives a section close. Acting on a stale
        // `{base, level}` after a concurrent growth means computing the wrong bit
        // index and writing the wrong slot on a node the writer holds a valid
        // claim in — packing turned that from a dangling dereference into a
        // stale-value hazard, but the re-decode-per-section discipline is the
        // same.
        struct Decoded {
            void*    root  = nullptr;
            unsigned level = 0;
            uint64_t base  = 0;    // absolute VA, zone base already folded in

            explicit operator bool() const { return root != nullptr; }
        };

        // ─── Encode / decode ───────────────────────────────────────────────

        static uint64_t encode(const void* root, unsigned level,
                               uint64_t clusterBase, size_t bucketIndex) {
            assert(root != nullptr, "radix bucket codec: an empty bucket is the zero word, "
                                    "not an encoded null");
            assert(level >= 1 && level <= G.levelCount,
                   "radix bucket codec: root level out of range");

            const uintptr_t addr = reinterpret_cast<uintptr_t>(root);
            const uintptr_t b    = BasePolicy::base();
            assert(addr >= b, "radix bucket codec: root below the substrate window base");
            const uintptr_t off = addr - b;
            assert(off < (uintptr_t{1} << kWindowBits),
                   "radix bucket codec: root beyond the substrate window");
            assert(off < BasePolicy::windowLimit(),
                   "radix bucket codec: root outside the substrate region this codec was "
                   "bound to — it would encode and decode cleanly while naming memory the "
                   "tree does not own");
            assert(off % kPointerAlignment == 0,
                   "radix bucket codec: root is not aligned to the width the pointer field "
                   "was derived from — the low bits it shifts away are not zero");

            const uint64_t zoneBase = static_cast<uint64_t>(bucketIndex) * kZoneSpan;
            assert(clusterBase >= zoneBase && clusterBase < zoneBase + kZoneSpan,
                   "radix bucket codec: cluster base outside its bucket's zone — DEC-033 "
                   "gives one cluster per bucket and the index is a prefix, so this is a "
                   "placement bug and not a representation limit");
            const uint64_t rel = clusterBase - zoneBase;
            assert(rel % kMinClusterSpan == 0,
                   "radix bucket codec: cluster base is not a multiple of minClusterSpan — "
                   "bases are span-aligned and spans only grow, so this cannot happen for a "
                   "representable cluster");

            const uint64_t word =
                  kBucketGuardBit
                | (static_cast<uint64_t>(level) << kLevelShift)
                | ((rel / kMinClusterSpan) << kOffsetShift)
                | ((static_cast<uint64_t>(off) >> kPointerShift) << kPointerFieldShift);

            // The guard bit's whole purpose, restated where a future "tidy the
            // low bits" edit would break it.
            assert(word != 0, "radix bucket codec: a populated entry must be non-zero");
            return word;
        }

        static Decoded decode(uint64_t word, size_t bucketIndex) {
            if (isEmpty(word)) return {};
            assert((word & kBucketGuardBit) != 0,
                   "radix bucket codec: a non-zero entry without its guard bit — the word "
                   "was not written by this codec");

            Decoded d;
            d.level = static_cast<unsigned>((word >> kLevelShift) & kLevelMask);
            const uint64_t rel = ((word >> kOffsetShift) & kOffsetMask) * kMinClusterSpan;
            d.base  = static_cast<uint64_t>(bucketIndex) * kZoneSpan + rel;
            const uint64_t off = ((word >> kPointerFieldShift) & kPointerMask) << kPointerShift;
            d.root  = reinterpret_cast<void*>(BasePolicy::base() + off);

            assert(d.level >= 1 && d.level <= G.levelCount,
                   "radix bucket codec: decoded root level out of range");
            return d;
        }
    };

    template <GeometryDescriptor G>
    using HarnessBucketCodec = BucketCodec<G, HarnessSlotBase>;

    // ─── The table ─────────────────────────────────────────────────────────
    //
    // Exactly one small page (DEC-102), which is what the derived bucket count
    // means. It carries NO state word and is never claimed, never marked and
    // never reclaimed: one cluster per bucket, no cluster removed before
    // teardown, and growth is a single compare-exchange on the packed word. The
    // claim protocol is a non-root mechanism throughout (§5.1), which is also
    // why the reclamation walk terminates at a cluster root.
    struct BucketTable {
        Atomic<uint64_t> entries[kBucketCount];

        BucketTable() {
            for (size_t i = 0; i < kBucketCount; i++) entries[i].store(0, kPrivateInit);
        }

        BucketTable(const BucketTable&)            = delete;
        BucketTable& operator=(const BucketTable&) = delete;
    };

    static_assert(sizeof(BucketTable) == arch::smallPageSize,
                  "the root bucket table is exactly one small page — DEC-102 derives the "
                  "bucket count from the page size and the 8 B entry width, so anything "
                  "else means one of the two moved");

    // The bucket a VA belongs to: the top `rootBits` of the address. A prefix,
    // not a hash (DEC-033) — no probing, no collisions, and growth confined to a
    // bucket can never collide with another cluster.
    template <GeometryDescriptor G>
    constexpr size_t bucketIndexFor(uint64_t va) {
        return static_cast<size_t>(va >> slotSpanBits(G, 0));
    }

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_BUCKET_CODEC_H
