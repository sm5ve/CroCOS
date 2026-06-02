//
// vmsmalloc-internal slab descriptor layout, size-class schema, and derived
// constants (vmsmalloc Phase 2 — DEC-028, DEC-044, DEC-045).
//
// This header is implementation-internal: it is *not* exported from
// kernel/include/mem/ and is only included by future vmsmalloc translation
// units. The public allocation API (VMSubstrate::make<T> / destroy<T>) is in
// kernel/include/mem/VMSubstrate.h.
//
// What lives here:
//   - kSlabDescriptorMagic (DEC-044): the 64-bit canary that vmsfree checks
//     to validate that a pointer's containing page is in fact a vmsmalloc slab.
//   - kSlabSizeClasses (DEC-003): the size-class array, mirror of
//     LibAlloc::InternalAllocator::slabSizeClasses (InternalAllocator.cpp:26).
//   - struct SlabDescriptorBase: the uniform 32 B prefix that sits at the
//     start of every slab page. Field layout pinned by P2-DEC-001.
//   - bookkeeperSize(kWordCount): a constexpr switch returning the size of
//     SlabBookkeeper<kWordCount*64> for kWordCount ∈ [1, 8] (P2-DEC-004).
//   - slotCount(c) / slot0Offset(c) / slotAlignment(c): per-class constexpr
//     accessors derived via the DEC-045 fixpoint.
//   - sizeClassFor(size): linear-scan mapping size -> class index, with
//     sentinel kNumSizeClasses for sizes that exceed the largest class
//     (P2-DEC-005 — Phase 5's DEC-029 large-request bypass consumes this).
//   - template <size_t N> struct SlabDescriptor: the per-class full
//     descriptor type = prefix + SlabBookkeeper<N>.
//
// No runtime code, no slab creation paths, no allocator state. Phase 3
// adds the VMSubstrate primitives; Phase 4 builds the entry points.
//

#ifndef CROCOS_VMSUBSTRATE_SLAB_H
#define CROCOS_VMSUBSTRATE_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <core/atomic.h>
#include <core/math.h>
#include <core/utility.h>
#include <liballoc/Slab.h>
#include <mem/NUMA.h>

namespace kernel::mm::vmsmalloc {

// ============================================================
// DEC-044: 64-bit magic constant.
//
// Properties enforced via static_assert below:
//   (a) bits 63:48 = 0x5DAB — non-canonical when interpreted as an AMD64
//       virtual address (rejects accidental pointer-as-magic collisions);
//   (b) leading byte 0x5D differs from the DEC-024 0xCC poison pattern.
// ============================================================

inline constexpr uint64_t kSlabDescriptorMagic = 0x5DAB5DABDE5CC9C0ULL;
static_assert((kSlabDescriptorMagic >> 48) == 0x5DAB,
              "kSlabDescriptorMagic must keep its non-canonical signature");
// Disjoint-from-poison check, expressed in two steps so the compiler doesn't
// classify the final comparison as tautological under -Wtautological-compare
// (the assertion IS true by design for the pinned magic — that's the point).
inline constexpr uint8_t kSlabDescriptorMagicLeadByte =
    static_cast<uint8_t>(kSlabDescriptorMagic >> 56);
inline constexpr uint8_t kDec024PoisonByte = 0xCCu;
static_assert(kSlabDescriptorMagicLeadByte != kDec024PoisonByte,
              "kSlabDescriptorMagic must not collide with DEC-024 poison byte");

// ============================================================
// DEC-003 size-class schema.
//
// Mirror of LibAlloc::InternalAllocator::slabSizeClasses at
// libraries/LibAlloc/InternalAllocator.cpp:26. The two arrays must stay
// in lockstep; a future refactor that moves InternalAllocator's array
// into a public LibAlloc header will let us replace this comment with a
// static_assert(equal_to_liballoc_array).
// ============================================================

inline constexpr ConstexprArray kSlabSizeClasses =
    { 8ul, 16, 32, 64, 96, 128, 256, 512 };
inline constexpr size_t kNumSizeClasses = kSlabSizeClasses.size();

static_assert(isArraySorted(kSlabSizeClasses),
              "kSlabSizeClasses must be sorted ascending for sizeClassFor");
static_assert(kSlabSizeClasses[kNumSizeClasses - 1] <= arch::smallPageSize,
              "Largest size class must not exceed a small page");

// ============================================================
// SlabDescriptorBase — 32 B uniform prefix at the head of every slab page.
//
// Field layout per P2-DEC-001:
//   0   uint64_t magic
//   8   SlabDescriptorBase* next         (Treiber linkage, head-only — DEC-041)
//  16   Atomic<SlabDescriptorBase*> chainNext   (intra-chain — DEC-034)
//  24   uint32_t chainDepth              (valid on chain head only)
//  28   numa::DomainID numaDomain        (uint16_t under the hood)
//  30   uint8_t sizeClass                (index into kSlabSizeClasses)
//  31   uint8_t _padding
//
// Magic at offset 0 keeps DEC-026's `desc->magic` check on the first cache
// line of the descriptor with no offset arithmetic.
// ============================================================

struct SlabDescriptorBase {
    uint64_t magic;
    SlabDescriptorBase* next;
    Atomic<SlabDescriptorBase*> chainNext;
    uint32_t chainDepth;
    numa::DomainID numaDomain;
    uint8_t sizeClass;
    uint8_t _padding[1];
};

static_assert(sizeof(SlabDescriptorBase) == 32,
              "SlabDescriptorBase must pack to 32 B (P2-DEC-001)");
static_assert(alignof(SlabDescriptorBase) == 8);
static_assert(offsetof(SlabDescriptorBase, magic) == 0);
static_assert(offsetof(SlabDescriptorBase, next) == 8);
static_assert(offsetof(SlabDescriptorBase, chainNext) == 16);
static_assert(offsetof(SlabDescriptorBase, chainDepth) == 24);
static_assert(offsetof(SlabDescriptorBase, numaDomain) == 28);
static_assert(offsetof(SlabDescriptorBase, sizeClass) == 30);
static_assert(sizeof(Atomic<SlabDescriptorBase*>) == 8,
              "Atomic<SlabDescriptorBase*> must be a single pointer wide");
static_assert(sizeof(numa::DomainID) == 2,
              "numa::DomainID must be 2 B for the SlabDescriptorBase layout");

// ============================================================
// Size-class accessors (P2-DEC-002): no slotSize denormalization — all
// per-descriptor lookups are constexpr-table accesses keyed by sizeClass.
// ============================================================

inline constexpr size_t slotSize(size_t c) {
    return kSlabSizeClasses[c];
}

// DEC-001 / DEC-022: power-of-two classes align to their own size;
// non-power-of-two classes align to alignof(max_align_t) = 16.
inline constexpr size_t slotAlignment(size_t c) {
    return (slotSize(c) & (slotSize(c) - 1)) == 0 ? slotSize(c) : size_t{16};
}

// ============================================================
// DEC-045 fixpoint: derive slotCount[c] given that SlabBookkeeper<N>'s
// size depends on N, but constexpr template instantiation requires N at
// compile time.
//
// bookkeeperSize(kWC) returns sizeof(SlabBookkeeper<kWC*64>) for kWC in
// [1, 8]. Two notes:
//
//   - InlineSplitBitmapStorage<W>'s alignas(64) on both bitmap halves
//     forces a fixed ~128 B floor regardless of W (for W ≤ 8). That
//     pushes sizeof(SlabBookkeeper<N>) up to 192 B for kWC=1 and 256 B
//     for kWC≥2 — visibly larger than a naive 8*W + counter estimate.
//
//   - The switch is upper-bounded at kWC=8 because DEC-003's smallest
//     class (slotSize=8) fits at most ~490 slots per page (kWC=8). A
//     future schema with kWC > 8 will fail the validateAllClasses
//     static_assert below.
// ============================================================

inline constexpr size_t bookkeeperSize(size_t kWordCount) {
    switch (kWordCount) {
        case 1: return sizeof(LibAlloc::SlabBookkeeper<64>);
        case 2: return sizeof(LibAlloc::SlabBookkeeper<128>);
        case 3: return sizeof(LibAlloc::SlabBookkeeper<192>);
        case 4: return sizeof(LibAlloc::SlabBookkeeper<256>);
        case 5: return sizeof(LibAlloc::SlabBookkeeper<320>);
        case 6: return sizeof(LibAlloc::SlabBookkeeper<384>);
        case 7: return sizeof(LibAlloc::SlabBookkeeper<448>);
        case 8: return sizeof(LibAlloc::SlabBookkeeper<512>);
        default: return 0;
    }
}

inline constexpr size_t slotCount(size_t c) {
    const size_t ss = slotSize(c);
    const size_t align = max(ss, size_t{16});
    // Step 1: upper-bound N assuming zero bookkeeper bytes.
    size_t nUpper = (arch::smallPageSize - sizeof(SlabDescriptorBase)) / ss;
    size_t kWC = divideAndRoundUp(nUpper, size_t{64});
    // Step 2: compute slot0 given bookkeeper at the upper-bound kWC.
    size_t book = bookkeeperSize(kWC);
    size_t slot0 = roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + book, align);
    size_t n = (arch::smallPageSize - slot0) / ss;
    // Step 3: if shrinking N dropped kWC, recompute one more time. The
    // iteration only ever shrinks kWC, so a single retry converges.
    if (divideAndRoundUp(n, size_t{64}) != kWC) {
        kWC = divideAndRoundUp(n, size_t{64});
        book = bookkeeperSize(kWC);
        slot0 = roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + book, align);
        n = (arch::smallPageSize - slot0) / ss;
    }
    return n;
}

inline constexpr size_t slot0Offset(size_t c) {
    const size_t align = max(slotSize(c), size_t{16});
    const size_t kWC = divideAndRoundUp(slotCount(c), size_t{64});
    return roundUpToNearestMultiple(sizeof(SlabDescriptorBase) + bookkeeperSize(kWC), align);
}

// ============================================================
// Per-descriptor read accessors (P2-DEC-002): table lookup keyed by the
// descriptor's sizeClass byte. These are how vmsfree's DEC-026 validation
// chain (Phase 5, ITEM-049) consumes the descriptor.
// ============================================================

inline constexpr size_t slotSize(const SlabDescriptorBase* d) {
    return slotSize(d->sizeClass);
}
inline constexpr size_t slotCount(const SlabDescriptorBase* d) {
    return slotCount(d->sizeClass);
}
inline constexpr size_t slot0Offset(const SlabDescriptorBase* d) {
    return slot0Offset(d->sizeClass);
}

// ============================================================
// Class-invariant validation: every class index satisfies the page-fit,
// alignment, non-zero-slot, and kWC-bounded constraints.
// ============================================================

namespace detail {
    template <size_t... Cs>
    constexpr bool validateAllClasses(index_sequence<Cs...>) {
        return ((slotCount(Cs) > 0) && ...)
            && ((slot0Offset(Cs) % slotAlignment(Cs) == 0) && ...)
            && ((slot0Offset(Cs) + slotCount(Cs) * slotSize(Cs) <= arch::smallPageSize) && ...)
            && ((slot0Offset(Cs) >= sizeof(SlabDescriptorBase) + bookkeeperSize(divideAndRoundUp(slotCount(Cs), size_t{64}))) && ...)
            && ((divideAndRoundUp(slotCount(Cs), size_t{64}) <= 8) && ...);
    }
}
static_assert(detail::validateAllClasses(make_index_sequence<kNumSizeClasses>{}),
              "DEC-045 fixpoint invariants violated for at least one class "
              "(slotCount must be > 0, page-fit, alignment, kWC ≤ 8)");

// ============================================================
// sizeClassFor: map a request size to its class index. Returns the
// sentinel kNumSizeClasses for sizes that exceed the largest class —
// Phase 5's DEC-029 large-request bypass dispatches on this sentinel.
// ============================================================

inline constexpr size_t sizeClassFor(size_t size) {
    for (size_t c = 0; c < kNumSizeClasses; c++) {
        if (kSlabSizeClasses[c] >= size) return c;
    }
    return kNumSizeClasses;
}

// ============================================================
// Per-class full descriptor: prefix + class-specific bookkeeper.
// Instantiated in-place by Phase 4's slab-creation slow path.
// ============================================================

template <size_t N>
struct SlabDescriptor : SlabDescriptorBase {
    LibAlloc::SlabBookkeeper<N> bookkeeper;
};

// ============================================================
// Phase 3 / Phase 5 — Per-domain shared-state storage
//
// `partial[d][c]` and `tuning[d][c]` live in a NUMA-distributed per-domain
// buffer. Phase 3 lays down the raw storage (init-time-only); Phase 5
// (P5-DEC-002) populates the partial slots with full
// `Core::ChainedTreiberStack` instances and consumes the tuning counters.
//
// The ChainedTreiberStack instance is parameterized by vmsmalloc's DEC-015
// head encoding, its linkage extractors, and its tuning Hooks — all of which
// stay file-scope-internal to vmsmalloc.cpp (Phase 5). So this header cannot
// name the concrete stack type. Instead each partial slot is an opaque,
// correctly-aligned `PartialStackStorage` blob; vmsmalloc.cpp placement-news a
// stack into it during vmsmallocLateInit and recovers it (via `launder`) in
// its `partialFor` accessor. `kPartialStackStorageBytes` is a generous fixed
// size; vmsmalloc.cpp static_asserts `sizeof(PartialStack)` fits.
//
// The per-CPU `Magazine[kNumSizeClasses]` array is *not* declared here —
// per the P7-DEC-010 amendment 2026-05-27 (referenced under Phase 4.5 in
// the canonical phase order) it folds into `kernel::CpuLocal`, accessed
// at runtime as `kernel::cpuLocal().magazines[c]`.
// ============================================================

// 64-byte cache-line alignment hardcoded per CLAUDE.md (arch::cacheLineSize
// is not exposed as a kernel-wide constant; documented value is 64 on AMD64).
inline constexpr size_t kVmsmallocCacheLine = 64;

// Opaque storage for one per-(domain, class) ChainedTreiberStack instance.
// A ChainedTreiberStack with a 64-bit head encoding occupies two cache lines
// (cache-line-isolated head + maxChainLength, plus the tuning Hooks pointer).
// vmsmalloc.cpp asserts sizeof(PartialStack) <= kPartialStackStorageBytes.
inline constexpr size_t kPartialStackStorageBytes = 2 * kVmsmallocCacheLine;
struct alignas(kVmsmallocCacheLine) PartialStackStorage {
    unsigned char bytes[kPartialStackStorageBytes];
};

struct alignas(kVmsmallocCacheLine) Magazine {
    SlabDescriptorBase* head;
    uint32_t depth;
    // (no tail per DEC-041 — chain head is the only externally-visible end.)
};

// Residual per-(domain, class) tuning counters. The magazine-depth knob `K`
// is now the ChainedTreiberStack's own `maxChainLength` (P5-DEC-002), so the
// earlier `currentK` field is gone; these are the contention / starvation
// signals the Phase-10 policy will read, plus its single-runner try-lock.
struct alignas(kVmsmallocCacheLine) MagazineTuning {
    Atomic<uint32_t> overflowCount;
    Atomic<uint32_t> starvationCount;
    Atomic<uint32_t> policyLock;
};

// Tuning bounds (provisional per P3-DEC-002 — Phase 10 revisits with
// RadixVM workload data). kInitialK seeds each stack's maxChainLength.
inline constexpr uint32_t kInitialK = 8;
inline constexpr uint32_t kMinK     = 2;
inline constexpr uint32_t kMaxK     = 64;
static_assert(kMinK <= kInitialK && kInitialK <= kMaxK);

// Per-domain buffer layout: PartialStackStorage array, then MagazineTuning
// array, each indexed by size-class.
inline constexpr size_t kPartialOffset = 0;
inline constexpr size_t kPartialBytes  = kNumSizeClasses * sizeof(PartialStackStorage);
inline constexpr size_t kTuningOffset  = roundUpToNearestMultiple(
    kPartialOffset + kPartialBytes, alignof(MagazineTuning));
inline constexpr size_t kTuningBytes   = kNumSizeClasses * sizeof(MagazineTuning);
inline constexpr size_t kPerDomainBufBytes = roundUpToNearestMultiple(
    kTuningOffset + kTuningBytes, arch::smallPageSize);

static_assert(kPerDomainBufBytes % arch::smallPageSize == 0,
              "kPerDomainBufBytes must be a whole number of pages");

// Upper cap on DomainID values we'll index. Real consumer hardware is
// single-NUMA-domain; multi-domain servers stay well under 64. The static
// array sized to this cap costs ~512 B in .bss — negligible.
inline constexpr size_t kMaxDomains = 64;

// Per-domain buffer base pointers (defined in VMSubstrateSlab.cpp). A
// `nullptr` entry means that DomainID is not CPU-bearing; DEC-018 / DEC-038
// guarantee that no descriptor's `numaDomain` will index a null slot.
extern void* perDomainBufs[kMaxDomains];

// Base of the per-domain PartialStackStorage array. vmsmalloc.cpp recovers a
// typed `PartialStack*` from `&partialStorageFor(d)[c]`.
inline PartialStackStorage* partialStorageFor(numa::DomainID d) {
    assert(perDomainBufs[d.value] != nullptr,
           "partialStorageFor: indexed domain has no per-domain buffer");
    return reinterpret_cast<PartialStackStorage*>(
        static_cast<uint8_t*>(perDomainBufs[d.value]) + kPartialOffset);
}

inline MagazineTuning* tuningFor(numa::DomainID d) {
    assert(perDomainBufs[d.value] != nullptr,
           "tuningFor: indexed domain has no per-domain buffer");
    return reinterpret_cast<MagazineTuning*>(
        static_cast<uint8_t*>(perDomainBufs[d.value]) + kTuningOffset);
}

// Init entry point — registered in general.icd under memory_management
// phase, depends_on VMSubstrate.
bool vmsmallocInit();

// ─── Phase 5 hooks into the Phase-3 init (defined in vmsmalloc.cpp) ───
//
// vmsmallocInit calls vmsmallocLateInit once the per-domain buffers exist:
// it publishes the VMSubstrate VA window (base/size) for the DEC-015 head
// encoding and placement-news a ChainedTreiberStack into every
// (CPU-bearing domain, class) partial slot with its Hooks bound to the
// matching tuning row. `vmsBase` is arenaVirtualBase(0); `vmsSize` is the
// span actually covered by the live arenas (asserted to fit the DEC-015
// page-offset budget).
void vmsmallocLateInit(uintptr_t vmsBase, size_t vmsSize);

} // namespace kernel::mm::vmsmalloc

#endif // CROCOS_VMSUBSTRATE_SLAB_H
