//
// vmsmalloc DEC-049 — the {192, 320} size classes added for the radix tree.
//
// The classes exist so radix nodes (160 B / 288 B structural) realise at
// 192 B / 320 B instead of the 256 B / 512 B the old schema charged, and they
// carry a *contractual* 64 B slot alignment so radix ITEM-055's 64 B-aligned
// state word fits without a size change.
//
// The static side of that contract is already enforced at compile time in
// VMSubstrateSlab.h (validateAllClasses checks each promised alignment is a
// power of two, divides the slot stride, and divides slot0Offset). What this
// file adds is the *dynamic* half: that vmsmalloc actually hands back pointers
// with that alignment from real slabs, across a whole slab's worth of slots and
// across the refill path — the layout arithmetic being right is not the same
// claim as the allocator honouring it.
//

#include "../../test.h"
#include <TestHarness.h>

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <VMSubstrateSlab.h>   // real
#include "mocks/MockCpuLocal.h"

using namespace CroCOSTest;
namespace vms = kernel::mm::vmsmalloc;
namespace VS  = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;

namespace {

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

struct Harness {
    Harness(size_t cpus = 1, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        kernel::test::bindThreadToCpu(0);
    }
    ~Harness() { VS::test::shutdown(); }
};

// Index of a class by its slot size. Deliberately a search rather than a
// hard-coded index: DEC-003's schema is explicitly retunable, and a test that
// pins index 6 to 192 B would start silently checking the wrong class the first
// time someone inserts a class below it.
constexpr size_t classOf(size_t bytes) {
    for (size_t c = 0; c < vms::kNumSizeClasses; c++) {
        if (vms::slotSize(c) == bytes) return c;
    }
    return vms::kNumSizeClasses;   // sentinel — the ASSERT below reports it
}

// The radix node types this schema change exists for (spec §5.1's realised
// column). Sizes only — the real types land with Phase 1; what matters here is
// that a 288 B / 64 B-aligned object is representable.
struct alignas(64) FakeNode32 { unsigned char bytes[288]; };
struct alignas(64) FakeNode16 { unsigned char bytes[160]; };

}  // namespace

// ─── The schema itself ─────────────────────────────────────────────────────

TEST(vmsmalloc_dec049_classes_exist) {
    ASSERT_TRUE(classOf(192) < vms::kNumSizeClasses);
    ASSERT_TRUE(classOf(320) < vms::kNumSizeClasses);
    // The whole-page bypass boundary is untouched — 512 stays the largest class
    // (DEC-049: "the whole-page bypass boundary is untouched").
    ASSERT_EQ(512u, vms::slotSize(vms::kNumSizeClasses - 1));
}

// DEC-049's rationale cites the packing as "20 slots per slab at 192 B, 11 at
// 320 B". The realised layout gives 19 and 11: at 192 B, DEC-001's slot-0
// formula aligns slot 0 to a 192-multiple (384, past the 32 B descriptor and the
// 192 B bookkeeper), leaving (4096-384)/192 = 19. The cited 20 is what a 64 B
// slot-0 alignment would give, which is not the formula DEC-049 says it is
// relying on ("DEC-001's slot-0 formula already lands every slot of these
// classes on a 64 B boundary" — i.e. no layout change). Pinned here so the
// figure is recorded from the build rather than from the prose; nothing depends
// on it being 20 (the count is a rationale figure, not a contract).
TEST(vmsmalloc_dec049_realised_packing) {
    ASSERT_EQ(19u, vms::slotCount(classOf(192)));
    ASSERT_EQ(11u, vms::slotCount(classOf(320)));
}

TEST(vmsmalloc_dec049_contractual_alignment_is_64) {
    ASSERT_EQ(64u, vms::slotAlignment(classOf(192)));
    ASSERT_EQ(64u, vms::slotAlignment(classOf(320)));
    // DEC-025 as amended: 96 deliberately stays the 16 B case even though its
    // realised layout would support more. Asserted so a future "tidy up
    // slotAlignment into a derived rule" edit fails here rather than silently
    // widening a promise.
    ASSERT_EQ(16u, vms::slotAlignment(classOf(96)));
}

// A radix node must land in its intended class, or every memory figure in the
// spec is wrong. This is the assertion DEC-076 ("nodes realise at 192 B/320 B")
// reduces to.
TEST(vmsmalloc_dec049_radix_nodes_land_in_their_classes) {
    ASSERT_EQ(192u, vms::slotSize(vms::sizeClassFor(sizeof(FakeNode16))));
    ASSERT_EQ(320u, vms::slotSize(vms::sizeClassFor(sizeof(FakeNode32))));
    // ...and make<T>'s DEC-025 static_assert accepts them. This is the clause
    // the alignment raise exists for: at the old 16 B non-pow2 default these
    // two lines would not compile.
    static_assert(alignof(FakeNode16) <= vms::slotAlignment(vms::sizeClassFor(sizeof(FakeNode16))));
    static_assert(alignof(FakeNode32) <= vms::slotAlignment(vms::sizeClassFor(sizeof(FakeNode32))));
}

// ─── The allocator honours it ──────────────────────────────────────────────

// Every slot of a full slab, for both new classes. A slot0-only check cannot
// see a stride that breaks alignment at slot 1, which is the realistic way this
// contract would fail.
TEST(vmsmalloc_dec049_every_slot_is_64B_aligned) {
    Harness h;
    for (size_t bytes : {size_t{192}, size_t{320}}) {
        const size_t c = classOf(bytes);
        const size_t n = vms::slotCount(c);
        ASSERT_TRUE(n > 0);

        // Two slabs' worth, so the check crosses the slab-creation slow path
        // rather than only exercising the first slab's slot 0.
        void* ptrs[64];
        const size_t count = (n * 2 < 64) ? n * 2 : 64;
        for (size_t k = 0; k < count; k++) {
            ptrs[k] = VS::vmsmalloc(bytes);
            ASSERT_TRUE(ptrs[k] != nullptr);
            ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(ptrs[k]) % 64u);
        }
        for (size_t k = 0; k < count; k++) VS::vmsfree(ptrs[k]);
    }
}

// make<T> / destroy<T> round-trip on an over-aligned type in each new class —
// the actual call shape the radix tree will use.
TEST(vmsmalloc_dec049_make_destroy_over_aligned_node) {
    Harness h;
    auto small = VS::make<FakeNode16>();
    auto big   = VS::make<FakeNode32>();
    ASSERT_TRUE(small);
    ASSERT_TRUE(big);
    ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(small.raw()) % alignof(FakeNode16));
    ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(big.raw())   % alignof(FakeNode32));
    VS::destroy(small);
    VS::destroy(big);
}

// A size that falls *between* the new classes rounds up to the right one, and
// nothing in the 129..192 or 257..320 windows escapes to the whole-page bypass.
TEST(vmsmalloc_dec049_intermediate_sizes_round_up) {
    ASSERT_EQ(192u, vms::slotSize(vms::sizeClassFor(129)));
    ASSERT_EQ(192u, vms::slotSize(vms::sizeClassFor(192)));
    ASSERT_EQ(256u, vms::slotSize(vms::sizeClassFor(193)));
    ASSERT_EQ(320u, vms::slotSize(vms::sizeClassFor(257)));
    ASSERT_EQ(320u, vms::slotSize(vms::sizeClassFor(320)));
    ASSERT_EQ(512u, vms::slotSize(vms::sizeClassFor(321)));
    // Above the largest class is the sentinel, not a class index.
    ASSERT_EQ(vms::kNumSizeClasses, vms::sizeClassFor(513));
}
