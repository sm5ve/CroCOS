//
// vmsmalloc Phase 2 — compile-time + runtime checks for the slab descriptor
// layout and the DEC-045 fixpoint that drives slotCount[c] / slot0Offset[c].
// Created by Spencer Martin on 5/28/26.
//
// What this file pins (any change is loud):
//   - The hand-computed slotCount[c] table for the DEC-003 schema.
//   - SlabDescriptorBase's 32 B layout (offsets + sizes + alignment).
//   - DEC-044 magic-constant properties (non-canonical AMD64 VA + disjoint
//     from the DEC-024 0xCC poison pattern).
//   - sizeClassFor's sentinel return path for sizes that exceed the largest
//     class (P2-DEC-005 — Phase 5's DEC-029 large-request bypass consumes it).
//
// The header itself carries these same static_asserts; duplicating here
// surfaces a change in test output rather than only in kernel build output.
//

#include "../../test.h"
#include <TestHarness.h>

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <mem/NUMA.h>
#include <VMSubstrateSlab.h>

using namespace CroCOSTest;
namespace vms = kernel::mm::vmsmalloc;

// ============================================================
// DEC-044 magic — non-canonical VA + disjoint from poison.
// ============================================================

static_assert(vms::kSlabDescriptorMagic == 0x5DAB5DABDE5CC9C0ULL);
static_assert((vms::kSlabDescriptorMagic >> 48) == 0x5DAB);
static_assert(((vms::kSlabDescriptorMagic >> 56) & 0xFFu) != 0xCCu);

// ============================================================
// SlabDescriptorBase layout (P2-DEC-001) — 32 B, six fields, no padding
// other than the explicit trailing 1 B.
// ============================================================

static_assert(sizeof(vms::SlabDescriptorBase) == 32);
static_assert(alignof(vms::SlabDescriptorBase) == 8);
static_assert(offsetof(vms::SlabDescriptorBase, magic)      == 0);
static_assert(offsetof(vms::SlabDescriptorBase, next)       == 8);
static_assert(offsetof(vms::SlabDescriptorBase, chainNext)  == 16);
static_assert(offsetof(vms::SlabDescriptorBase, chainDepth) == 24);
static_assert(offsetof(vms::SlabDescriptorBase, numaDomain) == 28);
static_assert(offsetof(vms::SlabDescriptorBase, sizeClass)  == 30);

// ============================================================
// kSlabSizeClasses schema mirrors LibAlloc::InternalAllocator at
// libraries/LibAlloc/InternalAllocator.cpp:26. Pin every entry so a
// schema drift is caught at compile time.
// ============================================================

// DEC-049 inserted the 192 B and 320 B radix-node classes, which shifted every
// index above 96. This block was not updated with that commit and its stale
// assertions broke the build of KernelTestRunner; re-pinned here against the
// 10-class schema.
static_assert(vms::kNumSizeClasses == 10);
static_assert(vms::kSlabSizeClasses[0] == 8);
static_assert(vms::kSlabSizeClasses[1] == 16);
static_assert(vms::kSlabSizeClasses[2] == 32);
static_assert(vms::kSlabSizeClasses[3] == 64);
static_assert(vms::kSlabSizeClasses[4] == 96);
static_assert(vms::kSlabSizeClasses[5] == 128);
static_assert(vms::kSlabSizeClasses[6] == 192);   // DEC-049, radix 16-valence node
static_assert(vms::kSlabSizeClasses[7] == 256);
static_assert(vms::kSlabSizeClasses[8] == 320);   // DEC-049, radix 32-valence node
static_assert(vms::kSlabSizeClasses[9] == 512);

// ============================================================
// slotSize / slotAlignment per class — DEC-001 alignment contract,
// amended by DEC-049 (192/320 promise 64 B).
// ============================================================

static_assert(vms::slotSize(size_t{0}) == 8   && vms::slotAlignment(0) == 8);
static_assert(vms::slotSize(1) == 16  && vms::slotAlignment(1) == 16);
static_assert(vms::slotSize(2) == 32  && vms::slotAlignment(2) == 32);
static_assert(vms::slotSize(3) == 64  && vms::slotAlignment(3) == 64);
static_assert(vms::slotSize(4) == 96  && vms::slotAlignment(4) == 16);  // non-pow2 → max_align_t
static_assert(vms::slotSize(5) == 128 && vms::slotAlignment(5) == 128);
static_assert(vms::slotSize(6) == 192 && vms::slotAlignment(6) == 64);  // DEC-049
static_assert(vms::slotSize(7) == 256 && vms::slotAlignment(7) == 256);
static_assert(vms::slotSize(8) == 320 && vms::slotAlignment(8) == 64);  // DEC-049
static_assert(vms::slotSize(9) == 512 && vms::slotAlignment(9) == 512);

// ============================================================
// DEC-045 fixpoint — pinned slotCount[c] values.
//
// These follow from sizeof(SlabBookkeeper<N>) under the current LibAlloc
// layout. Empirically the bookkeeper packs to 192 B for kWC=1..7 (the
// bitmap-storage trailing-alignment padding swallows the allocated /
// reserved counters via [[no_unique_address]]) and jumps to 256 B at
// kWC=8 (the alloc bitmap fills the full 64-byte slot, leaving no
// padding for the counters to tuck into).
//
// If any of these numbers changes, either (a) the DEC-003 schema moved,
// or (b) LibAlloc's bookkeeper grew/shrank — both are deliberate changes
// worth noticing.
// ============================================================

// D-001 (user-approved 2026-08-08): slot 0 aligns to the class's CONTRACTUAL
// alignment, not to `max(slotSize, 16)`. That moves slot 0 earlier for every
// class whose contract is lower than its slot size — the three non-power-of-two
// classes — and leaves the power-of-two classes exactly where they were.
static_assert(vms::slotCount(size_t{0}) == 476);  // slotSize=8,   kWC=8, book=256, slot0=288
static_assert(vms::slotCount(1) == 242);  // slotSize=16,  kWC=4, book=192, slot0=224
static_assert(vms::slotCount(2) == 121);  // slotSize=32,  kWC=2, book=192, slot0=224
static_assert(vms::slotCount(3) == 60);   // slotSize=64,  kWC=1, book=192, slot0=256
static_assert(vms::slotCount(4) == 40);   // slotSize=96,  align=16 → slot0=224 (was 288 → 39)
static_assert(vms::slotCount(5) == 30);   // slotSize=128, kWC=1, book=192, slot0=256
static_assert(vms::slotCount(6) == 20);   // slotSize=192, align=64 → slot0=256 (was 384 → 19)
static_assert(vms::slotCount(7) == 15);   // slotSize=256, kWC=1, book=192, slot0=256
static_assert(vms::slotCount(8) == 12);   // slotSize=320, align=64 → slot0=256 (was 320 → 11)
static_assert(vms::slotCount(9) == 7);    // slotSize=512, kWC=1, book=192, slot0=512

// Every per-class invariant the header already checks; duplicated for
// loud test-output coverage:
static_assert(vms::slot0Offset(size_t{0}) % vms::slotAlignment(0) == 0);
static_assert(vms::slot0Offset(1) % vms::slotAlignment(1) == 0);
static_assert(vms::slot0Offset(2) % vms::slotAlignment(2) == 0);
static_assert(vms::slot0Offset(3) % vms::slotAlignment(3) == 0);
static_assert(vms::slot0Offset(4) % vms::slotAlignment(4) == 0);
static_assert(vms::slot0Offset(5) % vms::slotAlignment(5) == 0);
static_assert(vms::slot0Offset(6) % vms::slotAlignment(6) == 0);
static_assert(vms::slot0Offset(7) % vms::slotAlignment(7) == 0);
static_assert(vms::slot0Offset(8) % vms::slotAlignment(8) == 0);
static_assert(vms::slot0Offset(9) % vms::slotAlignment(9) == 0);

static_assert(vms::slot0Offset(size_t{0}) + vms::slotCount(size_t{0}) * vms::slotSize(size_t{0}) <= arch::smallPageSize);
static_assert(vms::slot0Offset(9) + vms::slotCount(9) * vms::slotSize(9) <= arch::smallPageSize);

// ============================================================
// Per-descriptor read accessors — sizeClass-keyed table lookup.
// ============================================================

TEST(VMSubstrateSlab_DescriptorAccessors_LookupBySizeClass) {
    vms::SlabDescriptorBase d{};
    d.sizeClass = 3;  // 64-byte class
    ASSERT_EQ(size_t(64), vms::slotSize(&d));
    ASSERT_EQ(vms::slotCount(3), vms::slotCount(&d));
    ASSERT_EQ(vms::slot0Offset(3), vms::slot0Offset(&d));

    d.sizeClass = 9;  // 512-byte class — index 9 since DEC-049 inserted 192 and 320
    ASSERT_EQ(size_t(512), vms::slotSize(&d));
    ASSERT_EQ(size_t(7), vms::slotCount(&d));
}

// ============================================================
// sizeClassFor — edge cases. Size 0 is undefined at the vmsmalloc API
// level (DEC-023 will reject it in Phase 7); only sizes ≥ 1 are tested.
// ============================================================

TEST(VMSubstrateSlab_SizeClassFor_BoundaryWalk) {
    // Below smallest class → class 0
    ASSERT_EQ(size_t(0), vms::sizeClassFor(1));
    ASSERT_EQ(size_t(0), vms::sizeClassFor(7));
    ASSERT_EQ(size_t(0), vms::sizeClassFor(8));

    // First step
    ASSERT_EQ(size_t(1), vms::sizeClassFor(9));
    ASSERT_EQ(size_t(1), vms::sizeClassFor(16));

    // Mid range
    ASSERT_EQ(size_t(2), vms::sizeClassFor(17));
    ASSERT_EQ(size_t(2), vms::sizeClassFor(32));
    ASSERT_EQ(size_t(3), vms::sizeClassFor(33));
    ASSERT_EQ(size_t(3), vms::sizeClassFor(64));

    // Non-pow2 class straddle
    ASSERT_EQ(size_t(4), vms::sizeClassFor(65));
    ASSERT_EQ(size_t(4), vms::sizeClassFor(96));
    ASSERT_EQ(size_t(5), vms::sizeClassFor(97));
    ASSERT_EQ(size_t(5), vms::sizeClassFor(128));

    // The two DEC-049 radix classes, which sit between 128 and 512 and shifted
    // every index above them.
    ASSERT_EQ(size_t(6), vms::sizeClassFor(129));
    ASSERT_EQ(size_t(6), vms::sizeClassFor(192));
    ASSERT_EQ(size_t(7), vms::sizeClassFor(193));
    ASSERT_EQ(size_t(7), vms::sizeClassFor(256));
    ASSERT_EQ(size_t(8), vms::sizeClassFor(257));
    ASSERT_EQ(size_t(8), vms::sizeClassFor(320));

    // Upper end
    ASSERT_EQ(size_t(9), vms::sizeClassFor(321));
    ASSERT_EQ(size_t(9), vms::sizeClassFor(512));

    // Sentinel: exceeds largest class → kNumSizeClasses (Phase 5 DEC-029
    // routes these to VMSubstrate::allocPage()).
    ASSERT_EQ(vms::kNumSizeClasses, vms::sizeClassFor(513));
    ASSERT_EQ(vms::kNumSizeClasses, vms::sizeClassFor(4096));
    ASSERT_EQ(vms::kNumSizeClasses, vms::sizeClassFor(4097));
}

// ============================================================
// SlabDescriptor<N> composes SlabDescriptorBase + SlabBookkeeper<N>.
// ============================================================

TEST(VMSubstrateSlab_FullDescriptor_BookkeeperFollowsPrefix) {
    vms::SlabDescriptor<60> desc{};
    // Cross-check the base-subobject offset against the descriptor pointer.
    auto* base = static_cast<vms::SlabDescriptorBase*>(&desc);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&desc),
              reinterpret_cast<uintptr_t>(base));
    // The bookkeeper follows the prefix with natural alignment.
    const uintptr_t bookkeeperOff =
        reinterpret_cast<uintptr_t>(&desc.bookkeeper) -
        reinterpret_cast<uintptr_t>(base);
    ASSERT_GE(bookkeeperOff, sizeof(vms::SlabDescriptorBase));
}
