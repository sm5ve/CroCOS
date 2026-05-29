//
// vmsmalloc Phase 7 entry-point assertions + make<T> round-trip (Phase 8 harness).
//
// The deferred Phase-7 negative tests: in the harness the kernel `assert` macro
// throws CroCOSTest::AssertionFailure (mocks/kassert.h), so the debug-only
// entry-point checks (DEC-023 / DEC-004) are catchable via EXPECT_ASSERT_FAILURE.
// The DEC-014 forbidden-context check never fires in userspace (no interrupt
// context — currentCpuInterruptDepths is all-zeros), so it isn't exercised here.
//

#include "../../test.h"
#include <TestHarness.h>

#include <stddef.h>
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
numa::DomainID allToZero(arch::ProcessorID) { return numa::DomainID{0}; }
struct Harness {
    Harness() {
        VS::test::initialize(1, 1);
        numa::test::configure(1, 1, &allToZero);
        kernel::test::bindThreadToCpu(0);
    }
    ~Harness() { VS::test::shutdown(); }
};
}  // namespace

// DEC-023: vmsmalloc(0) asserts (size must be positive).
TEST(vmsmalloc_zero_size_asserts) {
    Harness h;
    EXPECT_ASSERT_FAILURE(VS::vmsmalloc(0));
}

// DEC-004: vmsmalloc(size > pageSize) asserts.
TEST(vmsmalloc_oversize_asserts) {
    Harness h;
    EXPECT_ASSERT_FAILURE(VS::vmsmalloc(arch::smallPageSize + 1));
}

// DEC-023: vmsfree(nullptr) asserts (pointer must be non-null).
TEST(vmsfree_nullptr_asserts) {
    Harness h;
    EXPECT_ASSERT_FAILURE(VS::vmsfree(nullptr));
}

// Positive: make<T> / destroy<T> round-trips through vmsmalloc/vmsfree for a
// well-aligned slab-class type (the make<T> alignment static_asserts are
// compile-time and were verified separately).
TEST(vmsmalloc_make_destroy_round_trip) {
    Harness h;
    struct alignas(16) Widget { uint64_t a; uint64_t b; uint64_t c; };  // 24 B -> 32 B class
    auto p = VS::make<Widget>();
    p->a = 0xAA; p->b = 0xBB; p->c = 0xCC;
    ASSERT_EQ(uint64_t(0xAA), p->a);
    ASSERT_TRUE(p.raw() != nullptr);
    VS::destroy(p);
}
