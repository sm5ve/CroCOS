//
// vmsmalloc DEC-048 — the failable allocation family (`vmsmallocTry` /
// `tryMake<T>`), and the property that makes it composable.
//
// DEC-048 is one sentence of contract and one sentence of justification, and
// the justification is the harder of the two to keep true:
//
//   "The machinery has no intermediate states that assume success (counter
//    bumps, refill pops and the eager-free walk all leave valid states), so
//    the null return composes."
//
// A test that only checks `vmsmallocTry` returns null when pages run out
// verifies the contract and none of the justification. So every case below
// fails an allocation and then keeps using the allocator: the interesting
// claim is not that the failing call returned null, it is that the *next* call
// behaves exactly as if the failure had never happened.
//
// Both panic sites the decision enumerates are covered, because the failure-
// modes table's single-site enumeration being incomplete is precisely what
// DEC-048 had to correct:
//
//   1. the DEC-018 fresh-slab slow path (`createFreshSlab` -> allocPage), and
//   2. the DEC-029 whole-page bypass (sizes above the largest slab class),
//      which is the one serving RadixVM's 4 KiB cluster root page.
//
// The scripted `setPageAllocFailAt` hook stands in for real exhaustion. Using
// genuine pool exhaustion instead would work but would test something weaker:
// after draining a 64 MiB pool there is no memory left to demonstrate recovery
// with, and recovery is the half of the claim that can regress.
//

#include "../../test.h"
#include <TestHarness.h>

#include <vector>

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <VMSubstrateSlab.h>   // real
#include "mocks/MockCpuLocal.h"
#include "DebugIntrospection.h"   // magazine / shared-stack snapshots

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
    ~Harness() {
        VS::test::setPageAllocFailAt(-1);   // never leak an armed hook into the next test
        VS::test::shutdown();
    }
};

// A size in the largest slab class, and one above it. `kNumSizeClasses - 1` is
// searched rather than written as 512 so a schema retune (DEC-003 says the
// schema is retunable) moves both of these together.
constexpr size_t kLargestSlabSize = vms::slotSize(vms::kNumSizeClasses - 1);
constexpr size_t kBypassSize      = kLargestSlabSize + 1;

// Drain the magazine and the shared stack for `c` so the NEXT allocation of
// that class is forced down createFreshSlab. Without this the fresh-slab site
// is unreachable: the first allocation of a class builds a slab with dozens of
// slots and every subsequent one is a magazine hit, so a test that just arms
// the hook and allocates would be checking the fast path and passing for the
// wrong reason.
//
// Returns the pointers it allocated; the caller frees them once the failure
// under test has been observed (freeing earlier would republish the slab and
// undo the drain).
void fillCurrentSlabs(size_t size, std::vector<void*>& out) {
    const size_t c = vms::sizeClassFor(size);
    // Allocate until the magazine reports empty at entry, i.e. until the next
    // call would have to refill-or-create. The introspection snapshot is the
    // same one Phase 8's tests use.
    for (size_t guard = 0; guard < 4096; guard++) {
        if (VS::test::magazineSnapshot(c).depth == 0 &&
            VS::test::partialStackSnapshot(numa::DomainID{0}, c).empty)
            return;
        void* p = VS::vmsmalloc(size);
        ASSERT_TRUE(p != nullptr);
        out.push_back(p);
    }
    ASSERT_TRUE(false);   // 4096 allocations without draining: the drain is broken
}

struct alignas(64) RadixNodeSized { unsigned char bytes[288]; };

}  // namespace

// ─── Site 1: the DEC-029 whole-page bypass ─────────────────────────────────

TEST(vmsmalloc_dec048_bypass_try_returns_null_on_exhaustion) {
    Harness h;
    VS::test::setPageAllocFailAt(0);            // fail from the very first try
    ASSERT_EQ(nullptr, VS::vmsmallocTry(kBypassSize));
}

TEST(vmsmalloc_dec048_bypass_recovers_after_a_failed_try) {
    Harness h;

    VS::test::setPageAllocFailAt(0);
    ASSERT_EQ(nullptr, VS::vmsmallocTry(kBypassSize));

    // The failed call took nothing and left nothing half-done, so disarming the
    // hook must restore the pre-failure behaviour exactly.
    VS::test::setPageAllocFailAt(-1);
    void* p = VS::vmsmallocTry(kBypassSize);
    ASSERT_TRUE(p != nullptr);
    VS::vmsfree(p);
    ASSERT_EQ(0u, VS::test::activePageCount());
}

// ─── Site 2: the DEC-018 fresh-slab slow path ──────────────────────────────

TEST(vmsmalloc_dec048_fresh_slab_try_returns_null_on_exhaustion) {
    Harness h;
    std::vector<void*> held;
    fillCurrentSlabs(kLargestSlabSize, held);

    VS::test::setPageAllocFailAt(0);
    ASSERT_EQ(nullptr, VS::vmsmallocTry(kLargestSlabSize));

    VS::test::setPageAllocFailAt(-1);
    for (auto* p : held) VS::vmsfree(p);
}

TEST(vmsmalloc_dec048_fresh_slab_recovers_after_a_failed_try) {
    Harness h;
    std::vector<void*> held;
    fillCurrentSlabs(kLargestSlabSize, held);

    const size_t c            = vms::sizeClassFor(kLargestSlabSize);
    const size_t pagesBefore  = VS::test::activePageCount();

    VS::test::setPageAllocFailAt(0);
    ASSERT_EQ(nullptr, VS::vmsmallocTry(kLargestSlabSize));

    // DEC-048's justification, checked rather than assumed: the failing attempt
    // reached createFreshSlab, so it had already run the magazine miss and the
    // shared-stack pop. Neither may have left residue.
    ASSERT_EQ(pagesBefore, VS::test::activePageCount());
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);

    VS::test::setPageAllocFailAt(-1);
    void* p = VS::vmsmallocTry(kLargestSlabSize);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(pagesBefore + 1, VS::test::activePageCount());

    VS::vmsfree(p);
    for (auto* q : held) VS::vmsfree(q);
}

// ─── The panicking contract is unchanged for every other caller ────────────

TEST(vmsmalloc_dec048_panicking_path_is_unaffected_by_the_hook) {
    Harness h;
    // setPageAllocFailAt gates tryAllocPage only. `vmsmalloc` keeps its DEC-012
    // contract — it never consults the failable primitive, so an armed hook is
    // invisible to it. (That the panicking path still *panics* on genuine
    // exhaustion is not testable here: the mock aborts the process rather than
    // throwing, by design.)
    VS::test::setPageAllocFailAt(0);
    void* p = VS::vmsmalloc(kBypassSize);
    ASSERT_TRUE(p != nullptr);
    VS::vmsfree(p);
}

// ─── tryMake<T>, the surface RadixVM actually allocates through ────────────

TEST(vmsmalloc_dec048_trymake_returns_a_null_safeptr) {
    Harness h;
    std::vector<void*> held;
    fillCurrentSlabs(sizeof(RadixNodeSized), held);

    VS::test::setPageAllocFailAt(0);
    auto p = VS::tryMake<RadixNodeSized>();
    ASSERT_TRUE(!p);
    ASSERT_EQ(nullptr, p.raw());

    VS::test::setPageAllocFailAt(-1);
    auto q = VS::tryMake<RadixNodeSized>();
    ASSERT_TRUE(static_cast<bool>(q));
    VS::destroy(q);
    for (auto* r : held) VS::vmsfree(r);
}

// A failed tryMake must not have run T's constructor. Checked with a type that
// records construction, because "returns null" and "returns null without having
// constructed anything into a page it then discarded" are different claims, and
// only the second one is safe for a T with side effects.
namespace {
    int gConstructed = 0;
    struct alignas(64) Counted {
        unsigned char pad[288];
        Counted() { gConstructed++; }
    };
}

TEST(vmsmalloc_dec048_failed_trymake_does_not_construct) {
    Harness h;
    std::vector<void*> held;
    fillCurrentSlabs(sizeof(Counted), held);

    gConstructed = 0;
    VS::test::setPageAllocFailAt(0);
    auto p = VS::tryMake<Counted>();
    ASSERT_TRUE(!p);
    ASSERT_EQ(0, gConstructed);

    VS::test::setPageAllocFailAt(-1);
    auto q = VS::tryMake<Counted>();
    ASSERT_EQ(1, gConstructed);
    VS::destroy(q);
    for (auto* r : held) VS::vmsfree(r);
}
