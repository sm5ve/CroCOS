//
// radix-tree harness — the fixture every radix test opens with.
//
// One mmap'd mock arena, a NUMA topology, CPU binding, and a clean oracle. RAII
// so a thrown assertion still tears the arena down; a leaked mmap region across
// tests would make the *next* test's accounting meaningless, which is the kind
// of failure that gets misattributed for an afternoon.
//

#ifndef CROCOS_RADIX_HARNESS_H
#define CROCOS_RADIX_HARNESS_H

#include <stddef.h>
#include <arch.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock (radix, with the DEC-052 oracle)

#include <MockCpuLocal.h>      // kernel::test::bindThreadToCpu (vmsmalloc mocks)

#include "mocks/RadixOracle.h"

namespace CroCOSTest::radix {

    namespace VS   = kernel::mm::VMSubstrate;
    namespace numa = kernel::numa;

    inline numa::DomainID allToDomainZero(arch::ProcessorID) { return numa::DomainID{0}; }

    struct Harness {
        explicit Harness(size_t cpus = 1, size_t domains = 1) {
            VS::test::initialize(cpus, domains);
            numa::test::configure(cpus, domains, &allToDomainZero);
            kernel::test::bindThreadToCpu(0);
            // Accounting and injection are process-global (they must be — the
            // counters are read from every CPU thread), so each fixture resets
            // them rather than trusting the previous test to have been tidy.
            resetAccounting();
            resetInjection();
        }

        ~Harness() {
            resetInjection();
            VS::test::shutdown();
        }

        Harness(const Harness&) = delete;
        Harness& operator=(const Harness&) = delete;
    };

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_HARNESS_H
