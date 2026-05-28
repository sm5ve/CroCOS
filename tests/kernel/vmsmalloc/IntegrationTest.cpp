//
// vmsmalloc Phase 8 — single-threaded integration scenarios.
//
// Exercises the composed vmsmalloc / vmsfree state machine (Phases 5-7) against
// the mmap-backed MockVMSubstrate. Each test runs on one bound CPU / one NUMA
// domain. Concurrency scenarios live in ConcurrentTest.cpp.
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
#include "DebugIntrospection.h"

using namespace CroCOSTest;
namespace vms = kernel::mm::vmsmalloc;
namespace VS  = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;

namespace {

constexpr numa::DomainID kDom0{0};

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

// RAII harness: mmap region + topology + thread binding; tears down (munmap)
// even if an assertion throws.
struct Harness {
    Harness(size_t cpus = 1, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        kernel::test::bindThreadToCpu(0);
    }
    ~Harness() { VS::test::shutdown(); }
};

inline uintptr_t pageBase(void* p) {
    return reinterpret_cast<uintptr_t>(p) & ~(static_cast<uintptr_t>(arch::smallPageSize) - 1);
}

}  // namespace

// 1. Fill a slab to Full, free one slot, realloc — the becameAvailable drain
//    returns exactly the freed slot (matches the kernel boot-smoke property).
TEST(vmsmalloc_becameAvailable_reuses_freed_slot) {
    Harness h;
    const size_t c = vms::kNumSizeClasses - 1;          // 512 B, few slots/slab
    const size_t n = vms::slotCount(c);
    void* last = nullptr;
    for (size_t k = 0; k < n; k++) last = VS::vmsmalloc(vms::slotSize(c));
    // The Full slab was popped off the magazine.
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);
    VS::vmsfree(last);                                  // Full -> Partial -> publish
    ASSERT_EQ(1u, VS::test::magazineSnapshot(c).depth);
    void* again = VS::vmsmalloc(vms::slotSize(c));
    ASSERT_EQ(last, again);
}

// 2. Allocating slotCount(c) slots fills exactly one slab; the next allocation
//    lands in a different page.
TEST(vmsmalloc_fills_one_slab_then_fresh) {
    Harness h;
    const size_t c = 2;                                 // 32 B
    const size_t n = vms::slotCount(c);
    void* first = VS::vmsmalloc(vms::slotSize(c));
    const uintptr_t slab = pageBase(first);
    for (size_t k = 1; k < n; k++) {
        ASSERT_EQ(slab, pageBase(VS::vmsmalloc(vms::slotSize(c))));
    }
    void* overflow = VS::vmsmalloc(vms::slotSize(c));   // slab was Full -> fresh slab
    ASSERT_NE(slab, pageBase(overflow));
}

// 3. The becameFull pop empties a singleton magazine (head -> chainNext == null).
TEST(vmsmalloc_becameFull_pop_empties_singleton_magazine) {
    Harness h;
    const size_t c = vms::kNumSizeClasses - 1;
    const size_t n = vms::slotCount(c);
    for (size_t k = 0; k < n - 1; k++) VS::vmsmalloc(vms::slotSize(c));
    ASSERT_EQ(1u, VS::test::magazineSnapshot(c).depth);  // Partial slab still cached
    VS::vmsmalloc(vms::slotSize(c));                     // last slot -> becameFull -> pop
    auto m = VS::test::magazineSnapshot(c);
    ASSERT_EQ(0u, m.depth);
    ASSERT_TRUE(m.head == nullptr);
}

// 4. DEC-029 whole-page bypass: page-aligned, no slab descriptor, round-trips
//    through the mock's page allocator.
TEST(vmsmalloc_dec029_whole_page_bypass) {
    Harness h;
    const size_t before = VS::test::activePageCount();
    void* p = VS::vmsmalloc(1024);                      // > largest class (512)
    ASSERT_EQ(uintptr_t(0), reinterpret_cast<uintptr_t>(p) & (arch::smallPageSize - 1));
    ASSERT_EQ(before + 1, VS::test::activePageCount());
    VS::vmsfree(p);
    ASSERT_EQ(before, VS::test::activePageCount());
}

// 5. Same-domain free of a Full slab prepends it to the local magazine.
TEST(vmsmalloc_same_domain_free_extends_magazine) {
    Harness h;
    const size_t c = vms::kNumSizeClasses - 1;
    const size_t n = vms::slotCount(c);
    void* slot = nullptr;
    for (size_t k = 0; k < n; k++) slot = VS::vmsmalloc(vms::slotSize(c));
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);  // Full slab off-magazine
    VS::vmsfree(slot);
    auto m = VS::test::magazineSnapshot(c);
    ASSERT_EQ(1u, m.depth);
    ASSERT_EQ(pageBase(slot), reinterpret_cast<uintptr_t>(m.head));
}

// 6 + 7. Driving maxChainLength becameAvailable pushes flushes the chain to the
//    shared stack; a subsequent refill pops the whole chain back.
TEST(vmsmalloc_flush_then_refill_round_trip) {
    Harness h;
    const size_t c = vms::kNumSizeClasses - 1;
    const size_t n = vms::slotCount(c);
    VS::test::setMaxChainLength(kDom0, c, 2);            // flush after 2 publishes

    // Fill two distinct slabs to Full, recording one slot in each.
    void* slotA = nullptr;
    void* slotB = nullptr;
    for (size_t k = 0; k < n; k++) slotA = VS::vmsmalloc(vms::slotSize(c));
    for (size_t k = 0; k < n; k++) slotB = VS::vmsmalloc(vms::slotSize(c));
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);

    VS::vmsfree(slotA);                                 // publish #1 -> depth 1
    ASSERT_EQ(1u, VS::test::magazineSnapshot(c).depth);
    VS::vmsfree(slotB);                                 // publish #2 -> depth 2 == K -> flush
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);
    auto ss = VS::test::partialStackSnapshot(kDom0, c);
    ASSERT_FALSE(ss.empty);
    ASSERT_EQ(2u, ss.topDepth);

    // Magazine empty -> next alloc refills the whole depth-2 chain (neither slab
    // is Empty, so the eager-free walk leaves both, draining the shared stack).
    // The same call's fast path then allocates from the head slab, which had a
    // single free slot, re-fulling it -> becameFull pop -> depth drops to 1.
    VS::vmsmalloc(vms::slotSize(c));
    ASSERT_EQ(1u, VS::test::magazineSnapshot(c).depth);
    ASSERT_TRUE(VS::test::partialStackSnapshot(kDom0, c).empty);   // whole chain refilled
}

// 8. Cross-domain free routes to the home-domain stack, not the local magazine.
TEST(vmsmalloc_cross_domain_free_routes_home) {
    Harness h(2, 2);                                    // 2 CPUs, 2 domains
    // Map cpu i -> domain i so CPU 0 is domain 0, CPU 1 is domain 1.
    numa::test::configure(2, 2, [](arch::ProcessorID i) {
        return numa::DomainID{static_cast<uint16_t>(i)};
    });
    kernel::test::bindThreadToCpu(0);                   // domain 0 owner

    const size_t c = vms::kNumSizeClasses - 1;
    const size_t n = vms::slotCount(c);
    void* slot = nullptr;
    for (size_t k = 0; k < n; k++) slot = VS::vmsmalloc(vms::slotSize(c));  // slab is domain-0

    // Re-bind this thread as CPU 1 (domain 1) and free the domain-0 slab.
    kernel::test::bindThreadToCpu(1);
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);                     // CPU1 magazine empty
    VS::vmsfree(slot);
    ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);                     // unchanged on CPU1
    ASSERT_FALSE(VS::test::partialStackSnapshot(kDom0, c).empty);          // landed on domain 0
}
