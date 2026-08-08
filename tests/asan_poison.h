//
// Manual ASan poisoning for harnesses with a custom allocator inside a mock
// arena (radix-tree DEC-052).
//
// The problem this exists for: every userspace harness in this tree backs its
// mock VMSubstrate with ONE mmap region, so from ASan's point of view the whole
// arena is a single live allocation. A write into a slab slot that was freed
// and recycled is a write into valid, addressable, unpoisoned memory — ASan
// sees nothing, and neither does LSan, which has no distinguishable footprint
// to miss. That blindness is stated as a hazard in radix-tree.md §10.
//
// __asan_poison_memory_region closes it: poison a slot when its object is
// destroyed, unpoison when a new object is constructed there, and any
// use-after-grace-period becomes an ordinary ASan report.
//
// Compiles to nothing without ASan, so the same sources build under TSan (which
// has no equivalent and does not need one — it is looking for a different class
// of defect).
//

#ifndef CROCOS_TEST_ASAN_POISON_H
#define CROCOS_TEST_ASAN_POISON_H

#include <stddef.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CROCOS_TEST_HAVE_ASAN 1
#  endif
#endif
#if !defined(CROCOS_TEST_HAVE_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define CROCOS_TEST_HAVE_ASAN 1
#endif

#if defined(CROCOS_TEST_HAVE_ASAN)
extern "C" void __asan_poison_memory_region(void const volatile* addr, size_t size);
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
#endif

namespace CroCOSTest {

    // True when the oracle has teeth. Tests that assert a poisoned access is
    // caught must skip themselves when this is false, rather than silently
    // passing — a green run of a toothless oracle is exactly the outcome
    // DEC-052 exists to prevent.
    inline constexpr bool kAsanPoisonActive =
#if defined(CROCOS_TEST_HAVE_ASAN)
        true;
#else
        false;
#endif

    inline void poisonRegion(void* p, size_t bytes) {
#if defined(CROCOS_TEST_HAVE_ASAN)
        if (p && bytes) __asan_poison_memory_region(p, bytes);
#else
        (void)p; (void)bytes;
#endif
    }

    inline void unpoisonRegion(void* p, size_t bytes) {
#if defined(CROCOS_TEST_HAVE_ASAN)
        if (p && bytes) __asan_unpoison_memory_region(p, bytes);
#else
        (void)p; (void)bytes;
#endif
    }

}  // namespace CroCOSTest

#endif  // CROCOS_TEST_ASAN_POISON_H
