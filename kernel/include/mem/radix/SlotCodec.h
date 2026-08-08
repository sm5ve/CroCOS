//
// radix-tree — the slot codec (§5.2; DEC-021 / 022 / 023 / 024 / 025 / 081).
//
// A slot is one 64-bit word and is exactly one of three things:
//
//     all-zero              empty
//     bit 0 set             encoded leaf — a Mapping* plus a covered sub-range
//     bit 0 clear, nonzero  encoded child-node pointer
//
// and the third line is only true because every encoded child word sets
// `kChildGuardBit` (DEC-081): a node allocated at window offset 0 would
// otherwise encode to all-zero and read as an EMPTY SLOT. The guard bit makes
// that collision unrepresentable rather than merely unlikely, which is why it
// is carried by BOTH instances — the alternatives (an allocator-side "offset 0
// is never handed out" reservation, or a +1 bias on the stored offset) either
// push a codec invariant into a foreign spec that cannot check it, or cost an
// add on every child decode at every level.
//
// ─── Why this is a template parameter (DEC-023) ────────────────────────────
//
// The core tree is parameterized on a codec, not on a fixed layout, because the
// pointer-compression assumptions are FALSE under test: the userspace harness
// allocates its mock arena wherever mmap puts it, where neither the kernel's
// VMSubstrate base nor its canonical-bit pattern holds. Two instances, one
// arithmetic — see the note on SlotCodec below.
//
// ─── Bit layout ────────────────────────────────────────────────────────────
//
//     bits  3:0                 tag
//                                 bit 0  1 = leaf, 0 = child
//                                 bit 1  kChildGuardBit — child words only
//                                 bits 2-3 reserved, held at zero (DEC-089
//                                          declined to spend them on protection)
//     bits  (W-1):4             window-relative pointer, in place
//     bits  63:W                leaf sub-range; child words hold zero here
//
// where W is the VMSubstrate window width (39 on amd64: root[510], 512 GiB).
// The pointer is stored IN PLACE rather than shifted — objects are >= 16 B
// aligned so bits 3:0 of the offset are already zero and the tag occupies them
// for free. That leaves 64 - W bits for the range, which is exactly DEC-024's
// 25-bit budget, and the geometry's rangeUnitBits derivation is what spends it.
//
// ─── The range encoding ────────────────────────────────────────────────────
//
// Inclusive-end (DEC-039), in units of the level's rangeUnit, relative to the
// slot's own base. Inclusive-end is not a style choice: a half-open range over
// N units needs N+1 distinct end values and therefore one more bit than the
// budget allows at C0. The cost is that an EMPTY range is inexpressible — which
// is correct, because the tree never holds an empty leaf (a slot covering
// nothing IS the empty word), so encoding one is a protocol violation and is
// debug-asserted rather than given a reserved encoding nothing would consume
// (DEC-081).
//
// The encoding is LEVEL-DEPENDENT — the same 64 bits mean different things at
// different levels, because the unit differs. §3.1 turns on this: a leaf in a
// C0 slot with range [0,0] covers 8 KiB, and after a shortfall subdivision the
// surviving C1 leaf covering 4 KiB has the same pointer bits, the same tag and
// the same [0,0] — a BIT-IDENTICAL WORD FOR A HALVED RANGE. Anything comparing
// raw slot words (a validity token, a revalidation) therefore compares equal on
// an address that was just unmapped, which is why DEC-051's token records the
// DECODED value and every entry point here takes the level explicitly.
//

#ifndef CROCOS_RADIX_SLOT_CODEC_H
#define CROCOS_RADIX_SLOT_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <mem/VMSubstrate.h>

#include <mem/radix/Geometry.h>

namespace kernel::mm::radix {

    // ─── Tag bits (DEC-021 / DEC-081) ──────────────────────────────────────

    inline constexpr uint64_t kLeafTagBit     = uint64_t{1} << 0;
    inline constexpr uint64_t kChildGuardBit  = uint64_t{1} << 1;
    inline constexpr uint64_t kTagMask        = 0xFull;

    // Both node allocations and Mapping records are at least 16 B aligned —
    // vmsmalloc DEC-022 gives 16 B for every class of at least 16 B, and the
    // radix node classes contractually give 64 B (DEC-049). The 8 B class is
    // only 8 B-aligned and no tree object uses it.
    inline constexpr size_t kSlotPointerAlignment = 16;

    // ─── Decoded forms ─────────────────────────────────────────────────────

    // A leaf's covered sub-range, in the level's units, inclusive at both ends.
    // Zero-initialised, and that is a release-build decision rather than a
    // style one. Both writers pass a `SubRange` by reference to the failable
    // `subRangeFor`; one of them checks the result and the other debug-asserts
    // it ("a fully-covered child slot's replacement is the whole span, which is
    // always expressible"). In release the assert compiles out, so a
    // hypothetical false return would leave the struct indeterminate and encode
    // a leaf from uninitialised stack — which is undefined behaviour rather than
    // the merely-wrong range the assert describes. LTO is what made this visible
    // (`-Werror=maybe-uninitialized` across the inlined call); the members were
    // uninitialised before and the release kernel simply had no cross-TU view.
    struct SubRange {
        uint32_t lo = 0;
        uint32_t hi = 0;   // inclusive

        constexpr bool operator==(const SubRange&) const = default;
    };

    enum class SlotKind : uint8_t { Empty, Leaf, Child };

    // ─── Base policies ─────────────────────────────────────────────────────
    //
    // The one thing the two codec instances genuinely differ in. §10 requires
    // each codec to "assert its own base and range at construction rather than
    // trust vas-layout.md to stay true"; binding the base through a policy is
    // what gives each instance a place to do that.
    //
    // Both instances run the SAME arithmetic. That is deliberate and is
    // stronger than shipping a literally-uncompressed harness codec — which
    // cannot exist anyway, since a 64-bit pointer plus a 24-bit range does not
    // fit in 64 bits. §10's "the harness is structurally blind to the kernel
    // codec's compression arithmetic" narrows to the BASE and the canonical-bit
    // fact, which is what actually differs, rather than covering the shift and
    // masking logic that does not.

    // Harness: the mock arena's base, wherever mmap put it. Bound once when the
    // arena is created.
    //
    // The LIMIT matters as much as the base, and for a reason specific to the
    // harness. The nominal window is 39 bits / 512 GiB in both instances (the
    // mock reports the production width deliberately, so the arithmetic is
    // identical), but the harness's arena is 64 MiB — so an address anywhere in
    // the 512 GiB above the arena encodes and decodes "successfully" while
    // naming memory the tree does not own. Checking against the ARENA's extent
    // rather than the nominal window is what makes §10's "each codec must assert
    // its own base and range" a real check here instead of a formality.
    struct HarnessSlotBase {
        static inline uintptr_t boundBase = 0;
        static inline uint64_t  boundLimit = 0;

        static void bind(uintptr_t base, uint64_t limitBytes) {
            assert(base != 0, "radix codec: harness base must be non-null");
            assert(base % kSlotPointerAlignment == 0,
                   "radix codec: harness base must be slot-pointer aligned");
            assert(limitBytes != 0, "radix codec: harness arena limit must be non-zero");
            boundBase = base;
            boundLimit = limitBytes;
        }

        static uintptr_t base() {
            assert(boundBase != 0,
                   "radix codec: harness base used before bind() — a slot encoded against "
                   "base 0 decodes to a wild pointer with no diagnostic");
            return boundBase;
        }

        static uint64_t windowLimit() { return boundLimit; }
    };

    // ─── The codec ─────────────────────────────────────────────────────────

    template <GeometryDescriptor G, typename Base>
    struct SlotCodec {
        // Re-exported so a consumer that must build a SIBLING codec over the
        // same window — the bucket codec is the one — can do it without being
        // told the base policy a second time. Two codecs disagreeing about where
        // the window starts is not a compile error, it is a wild pointer.
        using BasePolicy = Base;

        // The VMSubstrate VA window width. Both the kernel and the mock supply
        // this; the mock deliberately reports the production 39 so the harness
        // runs identical math with the upper offset bits simply staying zero.
        static constexpr unsigned kWindowBits = VMSubstrate::kWindowAddressBits;

        // Bits left above the pointer field for the sub-range.
        static constexpr unsigned kRangeBits = 64 - kWindowBits;

        // The codec must PROVIDE at least what the geometry SPENDS. Equality is
        // the shipped case (DEC-024's 25 bits, spent down to one spare at C0),
        // but a geometry is free to budget less than the codec offers — the
        // deliberately-starved test geometry does exactly that, to make §6.1's
        // shortfall rows reachable exhaustively. Budgeting MORE is the error
        // this catches: DEC-025's per-level unit derivation would then derive
        // units for a codec that cannot hold them, and the overflow lands in the
        // pointer field.
        static_assert(G.rangeBudgetBits <= kRangeBits,
                      "the geometry budgets more sub-range bits than the codec has above the "
                      "VMSubstrate window — DEC-025's unit derivation would overflow into the "
                      "pointer field");

        static constexpr uint64_t kPointerFieldMask =
            ((uint64_t{1} << kWindowBits) - 1) & ~kTagMask;
        static constexpr uint64_t kRangeFieldShift = kWindowBits;

        // ─── Classification ────────────────────────────────────────────────

        static constexpr bool isEmpty(uint64_t word) { return word == 0; }
        static constexpr bool isLeaf (uint64_t word) { return (word & kLeafTagBit) != 0; }
        static constexpr bool isChild(uint64_t word) {
            return word != 0 && (word & kLeafTagBit) == 0;
        }

        static constexpr SlotKind kindOf(uint64_t word) {
            if (word == 0)                  return SlotKind::Empty;
            if (word & kLeafTagBit)         return SlotKind::Leaf;
            return SlotKind::Child;
        }

        // ─── Pointer half ──────────────────────────────────────────────────

        static uint64_t encodePointer(const void* p) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
            const uintptr_t b = BasePolicy::base();
            assert(addr >= b, "radix codec: pointer below the substrate window base");
            const uintptr_t off = addr - b;
            assert(off < (uintptr_t{1} << kWindowBits),
                   "radix codec: pointer beyond the substrate window — the codec encodes a "
                   "VAS-layout fact and this is it failing");
            assert(off < BasePolicy::windowLimit(),
                   "radix codec: pointer outside the substrate region this codec was bound "
                   "to — it would encode and decode cleanly while naming memory the tree "
                   "does not own");
            assert(off % kSlotPointerAlignment == 0,
                   "radix codec: pointer is not 16 B aligned, so the tag bits would collide "
                   "with address bits");
            return static_cast<uint64_t>(off);
        }

        static void* decodePointer(uint64_t word) {
            return reinterpret_cast<void*>(BasePolicy::base() + (word & kPointerFieldMask));
        }

        // ─── Child words ───────────────────────────────────────────────────

        static uint64_t encodeChild(const void* node) {
            const uint64_t w = encodePointer(node) | kChildGuardBit;
            // The guard bit's whole purpose, restated as an assert so a future
            // "tidy up the tag bits" edit fails loudly rather than resurrecting
            // the offset-0 collision.
            assert(w != 0 && (w & kLeafTagBit) == 0,
                   "radix codec: an encoded child must be nonzero and untagged");
            return w;
        }

        static void* decodeChild(uint64_t word) {
            assert(isChild(word), "radix codec: decodeChild on a non-child word");
            return decodePointer(word);
        }

        // ─── Leaf words ────────────────────────────────────────────────────

        static constexpr unsigned endpointBits(unsigned level) {
            return rangeEndpointBits(G, level);
        }

        static constexpr uint64_t endpointMask(unsigned level) {
            return (uint64_t{1} << endpointBits(level)) - 1;
        }

        static uint64_t encodeLeaf(const void* mapping, SubRange r, unsigned level) {
            const unsigned eb = endpointBits(level);
            // DEC-081(b): the inclusive-end encoding cannot express an empty
            // range and never needs to. Encoding one is a protocol violation.
            assert(r.lo <= r.hi,
                   "radix codec: empty sub-range — the tree never holds a leaf covering "
                   "nothing; an empty span IS the empty slot word");
            assert(r.hi < (uint64_t{1} << eb),
                   "radix codec: sub-range endpoint exceeds this level's unit count");

            const uint64_t rangeField =
                (static_cast<uint64_t>(r.lo)) | (static_cast<uint64_t>(r.hi) << eb);
            assert((rangeField >> kRangeBits) == 0,
                   "radix codec: sub-range field overflows DEC-024's budget");

            return encodePointer(mapping) | kLeafTagBit | (rangeField << kRangeFieldShift);
        }

        static void* decodeLeaf(uint64_t word) {
            assert(isLeaf(word), "radix codec: decodeLeaf on a non-leaf word");
            return decodePointer(word);
        }

        static SubRange decodeRange(uint64_t word, unsigned level) {
            assert(isLeaf(word), "radix codec: decodeRange on a non-leaf word");
            const unsigned eb = endpointBits(level);
            const uint64_t rangeField = word >> kRangeFieldShift;
            return SubRange{
                static_cast<uint32_t>(rangeField & endpointMask(level)),
                static_cast<uint32_t>((rangeField >> eb) & endpointMask(level)),
            };
        }

        // A leaf covering its slot's entire span — the ordinary placement shape.
        static constexpr SubRange fullSpan(unsigned level) {
            return SubRange{0, static_cast<uint32_t>(rangeUnitCount(G, level) - 1)};
        }

        // ─── The `covers` hook (DEC-022 / DEC-023) ─────────────────────────
        //
        // Injected into descent rather than having the core hand a raw word back
        // for the caller to test: the predicate inlines at the call site, so the
        // core stays value-agnostic-modulo-one-predicate and pays nothing on the
        // hot path. It is also what resolves the common "unmapped VA" lookup
        // WITHOUT dereferencing the Mapping at all.
        static bool covers(uint64_t word, uint64_t keyWithinSlot, unsigned level) {
            if (!isLeaf(word)) return false;
            const SubRange r = decodeRange(word, level);
            const uint64_t unitBits = rangeUnitBits(G, level);
            const uint64_t unit = keyWithinSlot >> unitBits;
            return unit >= r.lo && unit <= r.hi;
        }

        // Absolute byte range a leaf covers, given its slot's base. The form
        // §3.1's validity token records, and the form every dispatch decision
        // below works in — level-relative units are what make raw-word
        // comparison wrong.
        static void absoluteRange(uint64_t word, uint64_t slotBase, unsigned level,
                                  uint64_t& lo, uint64_t& hi) {
            const SubRange r = decodeRange(word, level);
            const unsigned unitBits = rangeUnitBits(G, level);
            lo = slotBase + (static_cast<uint64_t>(r.lo) << unitBits);
            hi = slotBase + ((static_cast<uint64_t>(r.hi) + 1) << unitBits) - 1;
        }

        // Inverse of absoluteRange: the encodable sub-range covering exactly
        // [lo, hi]. Returns false when that range is NOT expressible at this
        // level's granularity — which is the §6.3 dispatch's expressibility
        // test, and must be exact in both directions: rounding outward maps
        // addresses the caller never asked for, rounding inward unmaps live
        // ones.
        static bool subRangeFor(uint64_t slotBase, unsigned level,
                                uint64_t lo, uint64_t hi, SubRange& out) {
            if (hi < lo) return false;
            const unsigned unitBits = rangeUnitBits(G, level);
            const uint64_t unitSize = uint64_t{1} << unitBits;
            const uint64_t relLo = lo - slotBase;
            const uint64_t relEnd = hi - slotBase + 1;          // exclusive
            if (relLo % unitSize != 0)  return false;
            if (relEnd % unitSize != 0) return false;
            out = SubRange{ static_cast<uint32_t>(relLo >> unitBits),
                            static_cast<uint32_t>((relEnd >> unitBits) - 1) };
            return true;
        }
    };

    // The instance the userspace harness runs. The kernel instance differs only
    // in its base policy (a constant derived from the VMSubstrate window rather
    // than a bound arena address) and lands with Phase 5, where a real kernel
    // build first exists to bind it.
    template <GeometryDescriptor G>
    using HarnessSlotCodec = SlotCodec<G, HarnessSlotBase>;

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_SLOT_CODEC_H
