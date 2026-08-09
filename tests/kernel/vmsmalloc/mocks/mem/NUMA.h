//
// vmsmalloc Phase 8 — mock <mem/NUMA.h> for the userspace integration harness.
//
// Shadows the real kernel header via include-path ordering. Provides only the
// surface vmsmalloc.cpp / VMSubstrateSlab.h actually use: kernel::numa::DomainID
// (binary-compatible with the real one — uint16_t value, sizeof == 2, so the
// SlabDescriptorBase::numaDomain static_assert holds) and a configurable
// numaPolicy().homeDomain(cpu). The real NUMAPolicy (flat arrays built from
// ACPI topology) is far heavier than the harness needs.
//

#ifndef CROCOS_MOCK_NUMA_H
#define CROCOS_MOCK_NUMA_H

#include <stdint.h>
#include <arch.h>

namespace kernel::numa {

    // ─── Upper cap on DomainID values an array may be indexed by ───────────
    //
    // Lived in vmsmalloc's implementation-internal `VMSubstrateSlab.h` until a
    // second subsystem needed it (the radix control-block freelist, which
    // partitions by domain so a recycled block keeps its placement). A cap on
    // DomainID belongs with DomainID; vmsmalloc keeps its own spelling as an
    // alias so no call site there changes.
    //
    // Real consumer hardware is single-NUMA-domain and multi-domain servers stay
    // well under 64, so an array sized to this costs a few hundred bytes of .bss
    // for a bound nothing realistic approaches.
    inline constexpr size_t kMaxDomains = 64;


struct DomainID {
    uint16_t value = UINT16_MAX;  // UINT16_MAX == null/invalid sentinel (matches real)

    constexpr explicit DomainID(uint16_t v) : value(v) {}
    constexpr DomainID() = default;

    bool operator==(const DomainID& o) const { return value == o.value; }
    bool operator!=(const DomainID& o) const { return value != o.value; }

    static DomainID null() { return DomainID{}; }
};

// Configurable mock policy. The mapping cpu -> domain is a plain function
// pointer set via test::configure; default maps every CPU to domain 0.
class NUMAPolicy {
public:
    using Mapping = DomainID (*)(arch::ProcessorID);

    DomainID homeDomain(arch::ProcessorID cpu) const { return mapping_(cpu); }
    size_t   domainCount() const { return domainCount_; }
    size_t   cpuCount() const { return cpuCount_; }

    // Test-only mutator (mock — no access control needed).
    void configure(size_t cpus, size_t domains, Mapping mapping) {
        cpuCount_ = cpus; domainCount_ = domains; mapping_ = mapping;
    }

private:
    Mapping mapping_     = [](arch::ProcessorID) { return DomainID{0}; };
    size_t  cpuCount_    = 1;
    size_t  domainCount_ = 1;
};

const NUMAPolicy& numaPolicy();

namespace test {
    // Configure the topology before any vmsmalloc call. `mapping` must be a
    // plain function (no captures) returning the home domain for a CPU.
    void configure(size_t cpus, size_t domains, NUMAPolicy::Mapping mapping);
}

} // namespace kernel::numa

#endif // CROCOS_MOCK_NUMA_H
