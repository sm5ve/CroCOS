//
// radix-tree — the node and its state word (§5.3; DEC-039 / 041 / 062 / 078).
//
// ─── The state word ────────────────────────────────────────────────────────
//
//     bits  31:0    claim bitmap      fetch_or to claim, fetch_and to release
//     bit   32      dying mark        unconditional fetch_or, under a whole-node claim
//     bits  38:33   occupancy count   fetch_add / fetch_sub
//     bits  63:39   spare             HELD AT ZERO
//
// **Field order here is a correctness decision, not a layout convenience.**
// Three properties depend on it and each is load-bearing:
//
//   1. **The word is zero exactly when the node is live, empty and unclaimed.**
//      That is why the spare bits are held at zero rather than merely unused.
//
//   2. **The mark sits BELOW the count.** Count arithmetic is +/-(1 << 33), so a
//      carry or borrow out of the count reaches only spare bits and can never
//      touch bit 32. Padding *above* the count would not help, and the reason
//      is counter-intuitive: a borrow propagates upward THROUGH ZEROS to the
//      nearest set bit, so with the mark above the count and set, one spurious
//      decrement at zero would clear it — a use-after-free on a node already
//      committed to reclamation, arrived at from an ordinary accounting slip two
//      mechanisms away.
//
//   3. **Detection is asymmetric, and the asymmetry runs the wrong way.** A
//      borrow sets bits 39-63 and is visible at the first slip. An OVERFLOW is
//      not: the field holds 0..63 while the legal maximum is 32, so 31 excess
//      increments pass silently at valence 32 — and 47 at valence 16, which is
//      four of the six non-root levels. The debug range assert is the ONLY
//      detector for over-counting, and there is none in release.
//
// Any field added above the count, or any reordering for alignment or
// readability, reintroduces the carry/borrow path into the mark.
//
// ─── Why valence is capped at 32 (DEC-039) ─────────────────────────────────
//
// At 32 slots the bitmap, a 0..32 count and the mark fit ONE 64-bit word, so
// every primitive is a single atomic on one location. At 64 the bitmap alone
// fills the word, the count and mark move to a second, no single atomic covers
// both, and the check-then-act race between insertion and reclamation returns.
// The size-class result (288 B rather than 544 B, which would land in
// vmsmalloc's forbidden 513 B..4 KiB gap) follows for free and is a
// consequence, not the reason.
//

#ifndef CROCOS_RADIX_NODE_H
#define CROCOS_RADIX_NODE_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <core/atomic.h>
#include <core/rcu/EpochDomain.h>   // Core::rcu::RetireHead

#include <mem/radix/Geometry.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    // ─── State-word field geometry ─────────────────────────────────────────

    namespace state {
        inline constexpr unsigned kClaimBitCount = 32;
        inline constexpr unsigned kMarkShift     = 32;
        inline constexpr unsigned kCountShift    = 33;
        inline constexpr unsigned kCountBitCount = 6;
        inline constexpr unsigned kSpareShift    = kCountShift + kCountBitCount;   // 39

        inline constexpr uint64_t kClaimMask = (uint64_t{1} << kClaimBitCount) - 1;
        inline constexpr uint64_t kMarkMask  = uint64_t{1} << kMarkShift;
        inline constexpr uint64_t kCountUnit = uint64_t{1} << kCountShift;
        inline constexpr uint64_t kCountMask =
            ((uint64_t{1} << kCountBitCount) - 1) << kCountShift;
        inline constexpr uint64_t kSpareMask = ~(kClaimMask | kMarkMask | kCountMask);

        // §11: "Static assert on field offsets — the mark must be at a lower bit
        // index than the count." Stated as the property, not as the numbers, so
        // it keeps meaning if the widths ever change.
        static_assert(kMarkShift < kCountShift,
                      "the dying mark must sit BELOW the occupancy count, or a count borrow "
                      "reaches it (DEC-041)");
        static_assert(kClaimBitCount == 32,
                      "the claim bitmap must cover the maximum valence (DEC-039 caps it at 32)");
        static_assert((kClaimMask & kMarkMask) == 0);
        static_assert((kMarkMask & kCountMask) == 0);
        static_assert((kCountMask & kSpareMask) == 0);
        static_assert((kClaimMask | kMarkMask | kCountMask | kSpareMask) == ~uint64_t{0});
        static_assert(kSpareShift == 39);

        // The count field is WIDER than the legal maximum, and that is the
        // detection asymmetry above. Asserted so nobody "tightens" it into
        // exactly 6 bits of 0..32 range and concludes overflow is now caught.
        static_assert((uint64_t{1} << kCountBitCount) > 32,
                      "the count field holds more than its legal maximum — over-counting is "
                      "undetectable in release and the debug range assert is the only detector");

        constexpr uint64_t claimsOf(uint64_t word)  { return word & kClaimMask; }
        constexpr bool     isMarked(uint64_t word)  { return (word & kMarkMask) != 0; }
        constexpr unsigned countOf(uint64_t word)   {
            return static_cast<unsigned>((word & kCountMask) >> kCountShift);
        }
        constexpr uint64_t spareOf(uint64_t word)   { return word & kSpareMask; }

        // The distinctive terminal state of a marked, retired node: it keeps its
        // claim bits and is retired holding them (§6.4, §7.4). A validator can
        // assert exactly this shape.
        constexpr uint64_t terminalWord(uint64_t mask) { return mask | kMarkMask; }
    }

    // ─── The node ──────────────────────────────────────────────────────────
    //
    // One concrete type per valence (DEC-062): {16, 32} under the amd64
    // geometry. The retire walk of a detached subtree or of teardown visits
    // nodes of mixed levels and needs a concrete T at every `retire` site, so it
    // dispatches on the node's LEVEL — which every walk already carries, since a
    // descent counts levels from the bucket entry — through the compile-time
    // level->valence map. That collapses to a two-way branch, and closing the
    // enumeration this way is exactly the validation RCU-DEC-016 was held
    // Provisional for.
    //
    // Header is 32 B: state word, refcount, and a 16 B inline RetireHead. The
    // refcount is UNCONDITIONAL on every node (DEC-078) — it is already inside
    // the 32 B header and the {192, 320} realised classes, so opting out saves
    // nothing (160 and 280 realise to the same classes) while forking the node
    // layout, the assert set and the size-class budget for zero bytes.
    //
    // Where the header sits relative to the slots is deliberately still open
    // (ITEM-055): the state word is written by every writer touching the node
    // and the slots are read by every reader descending through it, so
    // co-locating them false-shares the write onto a read-hot line while
    // separating them costs a second line on the write path. That is measurable
    // rather than arguable, and DEC-012 (node metadata is write-path-only —
    // descent needs the slot array and nothing else) is what leaves the choice
    // free. Header-first until Phase 5 measures it.
    template <GeometryDescriptor G, unsigned Valence>
    struct Node {
        static constexpr unsigned kValence = Valence;

        // The whole-node mask. Spelled from the valence, never `~0` — see
        // Geometry.h's valenceMask for why the wrong operand is a
        // mark-clearing bug rather than merely wasteful.
        static constexpr uint64_t kWholeNodeMask = (uint64_t{1} << Valence) - 1;
        static_assert(Valence <= state::kClaimBitCount,
                      "valence exceeds the claim bitmap (DEC-039 caps it at 32)");

        Atomic<uint64_t>      stateWord;
        Atomic<uint64_t>      refcount;
        Core::rcu::RetireHead head;
        Atomic<uint64_t>      slots[Valence];

        // `initialRefcount` is a parameter, not a default, because DEC-103 needs
        // both values and getting it wrong is silent in opposite directions. An
        // ordinary node linked by a claim-protected slot publish is constructed
        // at 0 and takes its structural +1 in the commit phase. A node published
        // by a BUCKET-WORD CAS — cluster growth's new parent, creation's empty
        // root — is constructed at 1: the node is private until the CAS wins, so
        // that is not a `+1` on a published object but the initial condition its
        // publication requires. A root observable at count 0 would let a descent
        // cache's paired increment/release take it 0 -> 1 -> 0 and destroy a
        // live node.
        explicit Node(uint64_t initialRefcount) {
            stateWord.store(0, kPrivateInit);
            refcount.store(initialRefcount, kPrivateInit);
            head.next = nullptr;
            head.deleter = nullptr;
            for (unsigned i = 0; i < Valence; i++) slots[i].store(0, kPrivateInit);
        }

        // §7.1 / invariant 24: **the destructor releases nothing at all, and
        // neither it nor the deleter ever recurses.** The distinction between
        // the two is not cosmetic — a node retired while a foreign CPU's descent
        // cache pins it is destroyed only when that cache turns over, which §7.4
        // says may never happen, so putting the Mapping release here would pin
        // every Mapping the node named, and transitively their VMObjects and
        // physical frames, behind a residue bound stated in NODES.
        //
        // Defaulted rather than written empty so nobody can later "just add one
        // line" to it without deleting this comment first.
        ~Node() = default;

        Node(const Node&)            = delete;
        Node& operator=(const Node&) = delete;

        // ─── Slot access ───────────────────────────────────────────────────
        //
        // Raw. Every READER-path load must go through the protected-link-load
        // primitive instead (invariant 21); these exist for the writer paths
        // that already hold a claim, and for single-threaded Phase 1 use.
        uint64_t slotRelaxed(unsigned i) const {
            assert(i < Valence, "radix node: slot index out of range");
            return slots[i].load(kQuiescedRead);
        }
    };

    // ─── Level -> concrete node type (DEC-062) ─────────────────────────────

    template <GeometryDescriptor G, unsigned Level>
    using NodeAt = Node<G, valence(G, Level)>;

    // ─── Type-erased access ────────────────────────────────────────────────
    //
    // A descent visits nodes of mixed valence and needs to read their state
    // words and slots without knowing which concrete type it is standing on.
    // The header is byte-identical across valences — same fields, same order,
    // same offsets — so a level-agnostic view over it is well-defined, and the
    // static_asserts below are what keep it that way if the header is ever
    // reordered (which ITEM-055 may yet do).
    //
    // This does NOT erase the type for allocation, destruction or `retire`:
    // those need a concrete T, and DEC-062's whole point is that the level a
    // walk already carries selects it at compile time through a two-way branch.
    // Only *access* is erased.
    inline constexpr size_t kNodeHeaderBytes = 32;

    class NodeRef {
        void* p = nullptr;
    public:
        NodeRef() = default;
        explicit NodeRef(void* raw) : p(raw) {}

        explicit operator bool() const { return p != nullptr; }
        void* raw() const { return p; }
        bool operator==(const NodeRef& o) const { return p == o.p; }

        Atomic<uint64_t>& stateWord() const {
            return *reinterpret_cast<Atomic<uint64_t>*>(static_cast<unsigned char*>(p) + 0);
        }
        Atomic<uint64_t>& refcount() const {
            return *reinterpret_cast<Atomic<uint64_t>*>(static_cast<unsigned char*>(p) + 8);
        }
        Core::rcu::RetireHead& retireHead() const {
            return *reinterpret_cast<Core::rcu::RetireHead*>(
                static_cast<unsigned char*>(p) + 16);
        }
        Atomic<uint64_t>& slot(unsigned i) const {
            return *(reinterpret_cast<Atomic<uint64_t>*>(
                         static_cast<unsigned char*>(p) + kNodeHeaderBytes) + i);
        }
    };

    namespace detail {
        template <GeometryDescriptor G>
        constexpr bool nodeHeaderLayoutIsUniform() {
            // Aliases because offsetof is a macro and `Node<G, 16>` contains a
            // comma the preprocessor splits on.
            using Narrow = Node<G, 16>;
            using Wide   = Node<G, 32>;
            return offsetof(Narrow, stateWord) == offsetof(Wide, stateWord)
                && offsetof(Narrow, refcount)  == offsetof(Wide, refcount)
                && offsetof(Narrow, head)      == offsetof(Wide, head)
                && offsetof(Narrow, slots)     == offsetof(Wide, slots)
                && offsetof(Narrow, stateWord) == 0
                && offsetof(Narrow, refcount)  == 8
                && offsetof(Narrow, head)      == 16
                && offsetof(Narrow, slots)     == kNodeHeaderBytes;
        }
    }
    static_assert(detail::nodeHeaderLayoutIsUniform<kAmd64Geometry>(),
                  "NodeRef's type-erased access assumes the 32 B header is byte-identical "
                  "across valences and that slots follow it immediately");

    // ─── Size and class checks (§5.1, DEC-076) ─────────────────────────────
    //
    // Structural 160 B / 288 B, realising at 192 B / 320 B under the classes
    // vmsmalloc DEC-049 added for this consumer. These are asserted rather than
    // trusted because every memory figure in §5.1, §7.4 and DEC-018 is computed
    // from the realised column: a node that silently grew past 320 B structural
    // lands in the 512 B class — a 60% jump — and past 512 B costs a whole page.
    namespace detail {
        template <GeometryDescriptor G>
        constexpr bool nodeSizesAreAsSpecified() {
            return sizeof(Node<G, 16>) == 160 && sizeof(Node<G, 32>) == 288;
        }
    }
    static_assert(detail::nodeSizesAreAsSpecified<kAmd64Geometry>(),
                  "radix node sizes must be 160 B / 288 B structural (32 B header + 8 B per "
                  "slot). A change here invalidates every memory figure in the spec.");

    static_assert(alignof(Node<kAmd64Geometry, 32>) <= 64,
                  "node alignment must fit the contractual 64 B slot alignment of the "
                  "DEC-049 classes");

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_NODE_H
