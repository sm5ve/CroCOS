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

static_assert(vms::kNumSizeClasses == 8);
static_assert(vms::kSlabSizeClasses[0] == 8);
static_assert(vms::kSlabSizeClasses[1] == 16);
static_assert(vms::kSlabSizeClasses[2] == 32);
static_assert(vms::kSlabSizeClasses[3] == 64);
static_assert(vms::kSlabSizeClasses[4] == 96);
static_assert(vms::kSlabSizeClasses[5] == 128);
static_assert(vms::kSlabSizeClasses[6] == 256);
static_assert(vms::kSlabSizeClasses[7] == 512);

// ============================================================
// slotSize / slotAlignment per class — DEC-001 alignment contract.
// ============================================================

static_assert(vms::slotSize(size_t{0}) == 8   && vms::slotAlignment(0) == 8);
static_assert(vms::slotSize(1) == 16  && vms::slotAlignment(1) == 16);
static_assert(vms::slotSize(2) == 32  && vms::slotAlignment(2) == 32);
static_assert(vms::slotSize(3) == 64  && vms::slotAlignment(3) == 64);
static_assert(vms::slotSize(4) == 96  && vms::slotAlignment(4) == 16);  // non-pow2 → max_align_t
static_assert(vms::slotSize(5) == 128 && vms::slotAlignment(5) == 128);
static_assert(vms::slotSize(6) == 256 && vms::slotAlignment(6) == 256);
static_assert(vms::slotSize(7) == 512 && vms::slotAlignment(7) == 512);

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

static_assert(vms::slotCount(size_t{0}) == 476);  // slotSize=8,   kWC=8, book=256
static_assert(vms::slotCount(1) == 242);  // slotSize=16,  kWC=4, book=192
static_assert(vms::slotCount(2) == 121);  // slotSize=32,  kWC=2, book=192
static_assert(vms::slotCount(3) == 60);   // slotSize=64,  kWC=1, book=192
static_assert(vms::slotCount(4) == 39);   // slotSize=96,  kWC=1, book=192
static_assert(vms::slotCount(5) == 30);   // slotSize=128, kWC=1, book=192
static_assert(vms::slotCount(6) == 15);   // slotSize=256, kWC=1, book=192
static_assert(vms::slotCount(7) == 7);    // slotSize=512, kWC=1, book=192

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

static_assert(vms::slot0Offset(size_t{0}) + vms::slotCount(size_t{0}) * vms::slotSize(size_t{0}) <= arch::smallPageSize);
static_assert(vms::slot0Offset(7) + vms::slotCount(7) * vms::slotSize(7) <= arch::smallPageSize);

// ============================================================
// Per-descriptor read accessors — sizeClass-keyed table lookup.
// ============================================================

TEST(VMSubstrateSlab_DescriptorAccessors_LookupBySizeClass) {
    vms::SlabDescriptorBase d{};
    d.sizeClass = 3;  // 64-byte class
    ASSERT_EQ(size_t(64), vms::slotSize(&d));
    ASSERT_EQ(vms::slotCount(3), vms::slotCount(&d));
    ASSERT_EQ(vms::slot0Offset(3), vms::slot0Offset(&d));

    d.sizeClass = 7;  // 512-byte class
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

    // Upper end
    ASSERT_EQ(size_t(6), vms::sizeClassFor(129));
    ASSERT_EQ(size_t(6), vms::sizeClassFor(256));
    ASSERT_EQ(size_t(7), vms::sizeClassFor(257));
    ASSERT_EQ(size_t(7), vms::sizeClassFor(512));

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
