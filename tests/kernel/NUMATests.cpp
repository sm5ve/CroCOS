//
// Unit tests for NUMATopology and NUMAPolicy
// Tests the NUMA abstraction in isolation from ACPI iterators.
// Created by Spencer Martin on 4/1/26.
//

#include "../test.h"
#include <TestHarness.h>

#include <mem/NUMA.h>
#include <core/ds/Vector.h>

using namespace kernel::numa;
using namespace kernel::mm;
using namespace CroCOSTest;

// Forward-declare arch mock helpers defined in ArchMocks.cpp
namespace arch { namespace testing {
    void setProcessorCount(size_t count);
    void resetProcessorState();
}}

// ============================================================================
// Helpers
// ============================================================================

// RAII: set the mock processor count for a test, restore 8 afterwards.
class NUMATestSetup {
public:
    explicit NUMATestSetup(size_t cpuCount = 4) {
        arch::testing::setProcessorCount(cpuCount);
    }
    ~NUMATestSetup() {
        arch::testing::resetProcessorState();
        arch::testing::setProcessorCount(8);
    }
};

static phys_memory_range makeRange(uint64_t start, uint64_t end) {
    return { phys_addr(start), phys_addr(end) };
}

// Typed empty sentinels (wrap EmptyIterable so each has a distinct static type).
static EmptyIterable<ProcessorAffinityEntry>   emptyProcs;
static EmptyIterable<MemoryRangeAffinityEntry> emptyMem;
static EmptyIterable<GenericInitiatorEntry>    emptyGen;
static EmptyIterable<LatencyEntry>             emptyLat;
static EmptyIterable<BandwidthEntry>           emptyBw;

// ============================================================================
// Construction — trivial topology
// ============================================================================

TEST(TrivialTopology_NoInput) {
    NUMATestSetup setup(1);
    auto topo = NUMATopology::build(emptyProcs, emptyMem, emptyGen);
    ASSERT_TRUE(topo.isTrivial());
    ASSERT_EQ(1u, topo.domainCount());
    ASSERT_FALSE(topo.hasLatencyInfo());
    ASSERT_FALSE(topo.hasBandwidthInfo());
}

// ============================================================================
// Construction — CPU affinity
// ============================================================================

TEST(SingleDomain_CpuAffinity) {
    NUMATestSetup setup(4);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, /*rawDomain=*/0, /*rawClock=*/0});
    procs.push({1, 0, 0});
    procs.push({2, 0, 0});
    procs.push({3, 0, 0});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    ASSERT_EQ(1u, topo.domainCount());
    ASSERT_TRUE(topo.isTrivial());

    // All CPUs should land in domain 0
    ASSERT_EQ(DomainID(0), topo.domainForCpu(0).id);
    ASSERT_EQ(DomainID(0), topo.domainForCpu(1).id);
    ASSERT_EQ(DomainID(0), topo.domainForCpu(2).id);
    ASSERT_EQ(DomainID(0), topo.domainForCpu(3).id);
}

TEST(TwoDomain_CpuAffinity) {
    NUMATestSetup setup(4);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, /*rawDomain=*/0, /*rawClock=*/0});
    procs.push({1, 0, 0});
    procs.push({2, /*rawDomain=*/1, /*rawClock=*/1});
    procs.push({3, 1, 1});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    ASSERT_EQ(2u, topo.domainCount());
    ASSERT_FALSE(topo.isTrivial());

    ASSERT_EQ(DomainID(0), topo.domainForCpu(0).id);
    ASSERT_EQ(DomainID(0), topo.domainForCpu(1).id);
    ASSERT_EQ(DomainID(1), topo.domainForCpu(2).id);
    ASSERT_EQ(DomainID(1), topo.domainForCpu(3).id);
}

// ============================================================================
// Construction — raw domain ID normalisation
// ============================================================================

// Raw ACPI domain IDs can be arbitrary integers; the topology normalises them
// to 0-based sequential IDs in encounter order.
TEST(DomainIdNormalisation) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, /*rawDomain=*/100, /*rawClock=*/0});
    procs.push({1, /*rawDomain=*/200, /*rawClock=*/0});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    ASSERT_EQ(2u, topo.domainCount());

    // CPU 0 was in raw domain 100 (encountered first → ID 0).
    // CPU 1 was in raw domain 200 (encountered second → ID 1).
    const auto& d0 = topo.domainForCpu(0);
    const auto& d1 = topo.domainForCpu(1);
    ASSERT_NE(d0.id, d1.id);
    ASSERT_EQ(DomainID(0), d0.id);
    ASSERT_EQ(DomainID(1), d1.id);
}

// ============================================================================
// Construction — memory affinity
// ============================================================================

TEST(MemoryAffinity_DomainForAddress) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;

    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    mems.push({makeRange(0x0000, 0x1000), /*rawDomain=*/0, MemoryType::Volatile});
    mems.push({makeRange(0x2000, 0x3000), /*rawDomain=*/1, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);

    const auto* dom0 = topo.domainForAddress(phys_addr(0x0500));
    const auto* dom1 = topo.domainForAddress(phys_addr(0x2500));
    ASSERT_TRUE(dom0 != nullptr);
    ASSERT_TRUE(dom1 != nullptr);
    ASSERT_EQ(DomainID(0), dom0->id);
    ASSERT_EQ(DomainID(1), dom1->id);
}

TEST(MemoryAffinity_AddressMiss) {
    NUMATestSetup setup(1);
    Vector<MemoryRangeAffinityEntry> mems;
    mems.push({makeRange(0x0000, 0x1000), 0, MemoryType::Volatile});
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});

    auto topo = NUMATopology::build(procs, mems, emptyGen);
    ASSERT_TRUE(topo.domainForAddress(phys_addr(0x5000)) == nullptr);
}

// ============================================================================
// Clock domains
// ============================================================================

TEST(ClockDomains_ShareClockDomain) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    // CPUs 0 and 1 share clock domain (raw=100), CPU 2 is in its own (raw=200).
    procs.push({0, 0, /*rawClock=*/100});
    procs.push({1, 0, /*rawClock=*/100});
    procs.push({2, 0, /*rawClock=*/200});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    ASSERT_TRUE(topo.shareClockDomain(0, 1));
    ASSERT_FALSE(topo.shareClockDomain(0, 2));
    ASSERT_FALSE(topo.shareClockDomain(1, 2));
}

// ============================================================================
// Domain role flags
// ============================================================================

TEST(DomainRoleFlags_InitiatorAndTarget) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    // Domain 0 has CPUs (initiator) and memory (target).
    // Domain 1 has only memory (target only).
    procs.push({0, 0, 0});
    procs.push({1, 0, 0});
    mems.push({makeRange(0x0000, 0x1000), 0, MemoryType::Volatile});
    mems.push({makeRange(0x2000, 0x3000), 1, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);

    const auto& d0 = topo.domain(DomainID(0));
    const auto& d1 = topo.domain(DomainID(1));

    ASSERT_TRUE(d0.isInitiatorDomain);
    ASSERT_TRUE(d0.isTargetDomain);
    ASSERT_FALSE(d1.isInitiatorDomain);
    ASSERT_TRUE(d1.isTargetDomain);
}

TEST(DomainRoleFlags_InitiatorFilter) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    procs.push({0, 0, 0});
    mems.push({makeRange(0x0000, 0x1000), 0, MemoryType::Volatile});
    mems.push({makeRange(0x2000, 0x3000), 1, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);

    size_t initiatorCount = 0;
    for (const auto& dom : topo.initiatorDomains())
        initiatorCount++;
    ASSERT_EQ(1u, initiatorCount);

    size_t targetCount = 0;
    for (const auto& dom : topo.targetDomains())
        targetCount++;
    ASSERT_EQ(2u, targetCount);
}

// ============================================================================
// AllMemoryRegions flat iteration
// ============================================================================

TEST(AllMemoryRegions_FlatIteration) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    mems.push({makeRange(0x0000, 0x1000), 0, MemoryType::Volatile});
    mems.push({makeRange(0x1000, 0x2000), 0, MemoryType::Volatile});
    mems.push({makeRange(0x4000, 0x8000), 1, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);

    size_t total = 0;
    for (const auto& r : topo.allMemoryRegions())
        total++;
    ASSERT_EQ(3u, total);
}

// ============================================================================
// Per-domain iteration helpers
// ============================================================================

TEST(PerDomainIteration) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    procs.push({0, 0, 0});
    procs.push({1, 0, 0});
    mems.push({makeRange(0x0000, 0x1000), 0, MemoryType::Volatile});
    mems.push({makeRange(0x1000, 0x2000), 0, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);

    size_t cpuCount = 0;
    for (const auto& cpu : topo.cpusInDomain(DomainID(0)))
        cpuCount++;
    ASSERT_EQ(2u, cpuCount);

    size_t memCount = 0;
    for (const auto& r : topo.memoryRegionsInDomain(DomainID(0)))
        memCount++;
    ASSERT_EQ(2u, memCount);
}

// ============================================================================
// SLIT relative latency
// ============================================================================

TEST(RelativeLatency_SLIT) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<LatencyEntry> lats;
    // rawInitiatorDomainId, rawTargetDomainId, readLatency, writeLatency, absolute
    lats.push({0, 0, 10, DISTANCE_NO_DATA, false});
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 0, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 1, 10, DISTANCE_NO_DATA, false});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    ASSERT_TRUE(topo.hasLatencyInfo());
    ASSERT_FALSE(topo.hasAbsoluteLatencyInfo());
    ASSERT_EQ(DataQualityLevel::Relative, topo.dataQuality().latency);

    auto lat00 = topo.latencyBetween(DomainID(0), DomainID(0));
    auto lat01 = topo.latencyBetween(DomainID(0), DomainID(1));
    ASSERT_TRUE(lat00.occupied());
    ASSERT_TRUE(lat01.occupied());
    ASSERT_EQ(10u, *lat00);
    ASSERT_EQ(20u, *lat01);
}

// ============================================================================
// HMAT absolute latency
// ============================================================================

TEST(AbsoluteLatency_HMAT) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<LatencyEntry> lats;
    // Absolute picosecond values (absolute=true)
    lats.push({0, 0,  5000, DISTANCE_NO_DATA, true});
    lats.push({0, 1, 40000, DISTANCE_NO_DATA, true});
    lats.push({1, 0, 40000, DISTANCE_NO_DATA, true});
    lats.push({1, 1,  5000, DISTANCE_NO_DATA, true});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    ASSERT_TRUE(topo.hasLatencyInfo());
    ASSERT_TRUE(topo.hasAbsoluteLatencyInfo());
    ASSERT_EQ(DataQualityLevel::Absolute, topo.dataQuality().latency);

    auto lat = topo.latencyBetween(DomainID(0), DomainID(1));
    ASSERT_TRUE(lat.occupied());
    ASSERT_EQ(40000u, *lat);
}

// ============================================================================
// Bandwidth data
// ============================================================================

TEST(BandwidthData) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<BandwidthEntry> bws;
    // rawInitiatorDomainId, rawTargetDomainId, readBandwidth, writeBandwidth
    bws.push({0, 0, 40000, DISTANCE_NO_DATA});
    bws.push({0, 1, 10000, DISTANCE_NO_DATA});
    bws.push({1, 0, 10000, DISTANCE_NO_DATA});
    bws.push({1, 1, 40000, DISTANCE_NO_DATA});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, emptyLat, bws);
    ASSERT_TRUE(topo.hasBandwidthInfo());
    ASSERT_EQ(DataQualityLevel::Absolute, topo.dataQuality().bandwidth);

    auto bw00 = topo.bandwidthBetween(DomainID(0), DomainID(0));
    auto bw01 = topo.bandwidthBetween(DomainID(0), DomainID(1));
    ASSERT_TRUE(bw00.occupied());
    ASSERT_TRUE(bw01.occupied());
    ASSERT_EQ(40000u, *bw00);
    ASSERT_EQ(10000u, *bw01);
}

// ============================================================================
// Read / write direction distinction
// ============================================================================

TEST(ReadWriteLatencyDirection) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<LatencyEntry> lats;
    // Asymmetric read/write latencies for 0→1
    lats.push({0, 1, /*readLatency=*/10000, /*writeLatency=*/20000, true});
    lats.push({1, 0, 10000, 20000, true});
    lats.push({0, 0,  5000,  5000, true});
    lats.push({1, 1,  5000,  5000, true});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);

    auto readLat  = topo.latencyBetween(DomainID(0), DomainID(1), AccessDirection::Read);
    auto writeLat = topo.latencyBetween(DomainID(0), DomainID(1), AccessDirection::Write);
    ASSERT_TRUE(readLat.occupied());
    ASSERT_TRUE(writeLat.occupied());
    ASSERT_EQ(10000u, *readLat);
    ASSERT_EQ(20000u, *writeLat);
}

TEST(ReadWriteBandwidthDirection) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<BandwidthEntry> bws;
    bws.push({0, 1, /*readBw=*/40000, /*writeBw=*/20000});
    bws.push({1, 0, 40000, 20000});
    bws.push({0, 0, 80000, 60000});
    bws.push({1, 1, 80000, 60000});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, emptyLat, bws);

    auto readBw  = topo.bandwidthBetween(DomainID(0), DomainID(1), AccessDirection::Read);
    auto writeBw = topo.bandwidthBetween(DomainID(0), DomainID(1), AccessDirection::Write);
    ASSERT_TRUE(readBw.occupied());
    ASSERT_TRUE(writeBw.occupied());
    ASSERT_EQ(40000u, *readBw);
    ASSERT_EQ(20000u, *writeBw);
}

// ============================================================================
// Absent data → empty Optional
// ============================================================================

TEST(LatencyBetween_ReturnsEmpty_WhenNoMatrix) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    ASSERT_FALSE(topo.latencyBetween(DomainID(0), DomainID(1)).occupied());
    ASSERT_FALSE(topo.bandwidthBetween(DomainID(0), DomainID(1)).occupied());
}

TEST(LatencyBetween_ReturnsEmpty_WhenCellIsNoData) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    // Only read latency provided; write latency left as DISTANCE_NO_DATA.
    Vector<LatencyEntry> lats;
    lats.push({0, 1, 15000, DISTANCE_NO_DATA, true});
    lats.push({0, 0,  5000, DISTANCE_NO_DATA, true});
    lats.push({1, 0, 15000, DISTANCE_NO_DATA, true});
    lats.push({1, 1,  5000, DISTANCE_NO_DATA, true});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);

    // Read latency should be present; write should be absent (no writeLatencyMatrix created).
    ASSERT_TRUE(topo.latencyBetween(DomainID(0), DomainID(1), AccessDirection::Read).occupied());
    ASSERT_FALSE(topo.latencyBetween(DomainID(0), DomainID(1), AccessDirection::Write).occupied());
}

// ============================================================================
// Domain ordering
// ============================================================================

TEST(DomainsOrderedByLatency_Relative) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    procs.push({2, 2, 0});

    Vector<LatencyEntry> lats;
    // From domain 0: local=10, d1=20, d2=30
    lats.push({0, 0, 10, DISTANCE_NO_DATA, false});
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false});
    lats.push({0, 2, 30, DISTANCE_NO_DATA, false});
    lats.push({1, 0, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 1, 10, DISTANCE_NO_DATA, false});
    lats.push({1, 2, 25, DISTANCE_NO_DATA, false});
    lats.push({2, 0, 30, DISTANCE_NO_DATA, false});
    lats.push({2, 1, 25, DISTANCE_NO_DATA, false});
    lats.push({2, 2, 10, DISTANCE_NO_DATA, false});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    auto order = topo.domainsOrderedByLatency(DomainID(0));

    ASSERT_EQ(3u, order.size());
    // Should be sorted ascending: d0 (10), d1 (20), d2 (30)
    ASSERT_EQ(DomainID(0), order[0].domainId);
    ASSERT_EQ(10u,  order[0].distance);
    ASSERT_EQ(DomainID(1), order[1].domainId);
    ASSERT_EQ(20u,  order[1].distance);
    ASSERT_EQ(DomainID(2), order[2].domainId);
    ASSERT_EQ(30u,  order[2].distance);
}

TEST(DomainsOrdered_InferredFallback) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    procs.push({2, 2, 0});

    // No latency data — inferred ordering: local first, all others equal.
    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    auto order = topo.domainsOrderedByLatency(DomainID(1));

    ASSERT_EQ(3u, order.size());
    // Domain 1 is local (distance 0) and should be first.
    ASSERT_EQ(DomainID(1), order[0].domainId);
    ASSERT_EQ(0u, order[0].distance);
    ASSERT_EQ(DataQualityLevel::Inferred, order[0].quality);
}

TEST(DomainsOrderedByBandwidth_Descending) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    procs.push({2, 2, 0});

    Vector<BandwidthEntry> bws;
    // From domain 0: local=80, d1=40, d2=20 (highest bandwidth first)
    bws.push({0, 0, 80, DISTANCE_NO_DATA});
    bws.push({0, 1, 40, DISTANCE_NO_DATA});
    bws.push({0, 2, 20, DISTANCE_NO_DATA});
    bws.push({1, 0, 40, DISTANCE_NO_DATA});
    bws.push({1, 1, 80, DISTANCE_NO_DATA});
    bws.push({1, 2, 20, DISTANCE_NO_DATA});
    bws.push({2, 0, 20, DISTANCE_NO_DATA});
    bws.push({2, 1, 20, DISTANCE_NO_DATA});
    bws.push({2, 2, 80, DISTANCE_NO_DATA});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, emptyLat, bws);
    auto order = topo.domainsOrderedByBandwidth(DomainID(0));

    ASSERT_EQ(3u, order.size());
    // Descending by bandwidth: d0 (80), d1 (40), d2 (20)
    ASSERT_EQ(DomainID(0), order[0].domainId);
    ASSERT_EQ(80u, order[0].distance);
    ASSERT_EQ(DomainID(1), order[1].domainId);
    ASSERT_EQ(40u, order[1].distance);
    ASSERT_EQ(DomainID(2), order[2].domainId);
    ASSERT_EQ(20u, order[2].distance);
}

// ============================================================================
// Preferred initiator domain (memory-only domains)
// ============================================================================

TEST(PreferredInitiator_SingleInitiator) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    // Domain 0: CPU-only (initiator).  Domain 1: memory-only (target).
    procs.push({0, /*rawDomain=*/0, 0});
    procs.push({1, /*rawDomain=*/0, 0});
    mems.push({makeRange(0x0000, 0x1000), /*rawDomain=*/1, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);
    const auto& memDomain = topo.domain(DomainID(1));
    ASSERT_TRUE(memDomain.preferredInitiatorDomain.occupied());
    ASSERT_EQ(DomainID(0), *memDomain.preferredInitiatorDomain);
}

TEST(PreferredInitiator_MultipleInitiators) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry>   procs;
    Vector<MemoryRangeAffinityEntry> mems;
    // Domains 0 and 1 both have CPUs; domain 2 is memory-only.
    // With multiple initiators, no preferred should be assigned.
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    procs.push({2, 1, 0});
    mems.push({makeRange(0x0000, 0x1000), 2, MemoryType::Volatile});

    auto topo = NUMATopology::build(procs, mems, emptyGen);
    const auto& memDomain = topo.domain(DomainID(2));
    ASSERT_FALSE(memDomain.preferredInitiatorDomain.occupied());
}

// ============================================================================
// CPU coverage validation
// ============================================================================

// CPUs not mentioned in SRAT should be silently assigned to domain 0.
TEST(CpuCoverageValidation) {
    NUMATestSetup setup(4);
    Vector<ProcessorAffinityEntry> procs;
    // Only CPUs 0 and 1 mentioned; CPUs 2 and 3 will be added by coverage check.
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);

    // CPUs 2 and 3 should resolve to domain 0 (the fallback).
    ASSERT_EQ(DomainID(0), topo.domainForCpu(2).id);
    ASSERT_EQ(DomainID(0), topo.domainForCpu(3).id);
}

// ============================================================================
// Duplicate latency entry handling
// ============================================================================

TEST(DuplicateLatency_ConflictKeepsFirst) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<LatencyEntry> lats;
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false}); // first
    lats.push({0, 1, 99, DISTANCE_NO_DATA, false}); // conflict — should be ignored
    lats.push({0, 0, 10, DISTANCE_NO_DATA, false});
    lats.push({1, 0, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 1, 10, DISTANCE_NO_DATA, false});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    auto lat = topo.latencyBetween(DomainID(0), DomainID(1));
    ASSERT_TRUE(lat.occupied());
    ASSERT_EQ(20u, *lat); // first value kept
}

TEST(DuplicateLatency_SameValue_Silent) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    Vector<LatencyEntry> lats;
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false});
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false}); // same value — should be silent
    lats.push({0, 0, 10, DISTANCE_NO_DATA, false});
    lats.push({1, 0, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 1, 10, DISTANCE_NO_DATA, false});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    auto lat = topo.latencyBetween(DomainID(0), DomainID(1));
    ASSERT_TRUE(lat.occupied());
    ASSERT_EQ(20u, *lat);
}

// ============================================================================
// NUMAPolicy — basic queries
// ============================================================================

TEST(NUMAPolicy_HomeDomain) {
    NUMATestSetup setup(4);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 0, 0});
    procs.push({2, 1, 0});
    procs.push({3, 1, 0});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    NUMAPolicy policy(topo);

    ASSERT_EQ(DomainID(0), policy.homeDomain(0));
    ASSERT_EQ(DomainID(0), policy.homeDomain(1));
    ASSERT_EQ(DomainID(1), policy.homeDomain(2));
    ASSERT_EQ(DomainID(1), policy.homeDomain(3));
}

TEST(NUMAPolicy_ClockDomain) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, /*rawClock=*/0});
    procs.push({1, 0, /*rawClock=*/1});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    NUMAPolicy policy(topo);

    ASSERT_NE(policy.clockDomain(0), policy.clockDomain(1));
}

TEST(NUMAPolicy_ShareClockDomain) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, /*rawClock=*/0});
    procs.push({1, 0, /*rawClock=*/0});
    procs.push({2, 0, /*rawClock=*/1});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    NUMAPolicy policy(topo);

    ASSERT_TRUE(policy.shareClockDomain(0, 1));
    ASSERT_FALSE(policy.shareClockDomain(0, 2));
}

TEST(NUMAPolicy_DomainOrder_LocalFirst) {
    NUMATestSetup setup(2);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});

    // No latency data → inferred ordering: local first.
    auto topo = NUMATopology::build(procs, emptyMem, emptyGen);
    NUMAPolicy policy(topo);

    auto order0 = policy.domainOrder(DomainID(0));
    ASSERT_EQ(2u, order0.size());
    ASSERT_EQ(DomainID(0), order0[0]); // local first

    auto order1 = policy.domainOrder(DomainID(1));
    ASSERT_EQ(2u, order1.size());
    ASSERT_EQ(DomainID(1), order1[0]); // local first
}

TEST(NUMAPolicy_DomainOrder_ByLatency) {
    NUMATestSetup setup(3);
    Vector<ProcessorAffinityEntry> procs;
    procs.push({0, 0, 0});
    procs.push({1, 1, 0});
    procs.push({2, 2, 0});

    Vector<LatencyEntry> lats;
    lats.push({0, 0, 10, DISTANCE_NO_DATA, false});
    lats.push({0, 1, 20, DISTANCE_NO_DATA, false});
    lats.push({0, 2, 30, DISTANCE_NO_DATA, false});
    lats.push({1, 0, 20, DISTANCE_NO_DATA, false});
    lats.push({1, 1, 10, DISTANCE_NO_DATA, false});
    lats.push({1, 2, 25, DISTANCE_NO_DATA, false});
    lats.push({2, 0, 30, DISTANCE_NO_DATA, false});
    lats.push({2, 1, 25, DISTANCE_NO_DATA, false});
    lats.push({2, 2, 10, DISTANCE_NO_DATA, false});

    auto topo = NUMATopology::build(procs, emptyMem, emptyGen, lats, emptyBw);
    NUMAPolicy policy(topo);

    // From domain 1: order should be 1 (10), 0 (20), 2 (25)
    auto order1 = policy.domainOrder(DomainID(1));
    ASSERT_EQ(3u, order1.size());
    ASSERT_EQ(DomainID(1), order1[0]);
    ASSERT_EQ(DomainID(0), order1[1]);
    ASSERT_EQ(DomainID(2), order1[2]);
}
