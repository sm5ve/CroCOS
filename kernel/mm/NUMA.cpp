//
// Created by Spencer Martin on 3/28/26.
//

#include <mem/NUMA.h>
#include <core/ds/HashMap.h>
#include <core/ds/HashSet.h>
#include <core/math.h>
#include <kernel.h>
#include <assert.h>

namespace kernel::numa {

using kernel::klog;

// ============================================================================
// NormMap — normalises raw ACPI domain IDs to sequential indices
// ============================================================================

template<typename ID>
struct NormMap {
    HashMap<uint32_t, ID> map;
    size_t nextId = 0;

    ID get(uint32_t raw) {
        ID existing;
        if (map.get(raw, existing)) return existing;
        auto id = ID(static_cast<uint16_t>(nextId++));
        map.insert(raw, id);
        return id;
    }
    bool contains(uint32_t raw) const {
        ID unused;
        return map.get(raw, unused);
    }
    size_t count() const { return nextId; }
};

// ============================================================================
// buildInternal — core construction logic
// ============================================================================

NUMATopology NUMATopology::buildInternal(
    Vector<ProcessorAffinityEntry>   procs,
    Vector<MemoryRangeAffinityEntry> mems,
    Vector<GenericInitiatorEntry>    gens,
    Vector<LatencyEntry>             lats,
    Vector<BandwidthEntry>           bws)
{
    // Trivial case: no affinity data at all.  Return a single domain topology.
    if (procs.empty() && mems.empty() && gens.empty()) {
        NUMATopology topo;
        ProximityDomain d;
        d.id = DomainID(0);
        topo.proximityDomains.push(move(d));
        topo.cacheInfo.push(Optional<DomainCacheInfo>{});
        klog() << "[NUMA] No SRAT data — trivial single-domain topology\n";
        return topo;
    }

    NormMap<DomainID>      domainMap;
    NormMap<ClockDomainID> clockMap;
    size_t maxCpuId = 0;

    // -------------------------------------------------------------------------
    // Pass 1 — collect all unique raw IDs and determine table sizes.
    // -------------------------------------------------------------------------
    for (const auto& e : procs) {
        domainMap.get(e.rawDomainId);
        clockMap.get(e.rawClockDomainId);
        const size_t cpuIdx = static_cast<size_t>(e.cpuId);
        if (cpuIdx > maxCpuId) maxCpuId = cpuIdx;
    }
    for (const auto& e : mems) domainMap.get(e.rawDomainId);
    for (const auto& e : gens) domainMap.get(e.rawDomainId);

    // Latency/bandwidth entries may reference domains not seen elsewhere;
    // add them (unusual but gracefully handled per spec).
    for (const auto& e : lats) {
        if (!domainMap.contains(e.rawInitiatorDomainId) ||
            !domainMap.contains(e.rawTargetDomainId)) {
            klog() << "[NUMA] Warning: latency entry references undeclared domain\n";
        }
        domainMap.get(e.rawInitiatorDomainId);
        domainMap.get(e.rawTargetDomainId);
    }
    for (const auto& e : bws) {
        if (!domainMap.contains(e.rawInitiatorDomainId) ||
            !domainMap.contains(e.rawTargetDomainId)) {
            klog() << "[NUMA] Warning: bandwidth entry references undeclared domain\n";
        }
        domainMap.get(e.rawInitiatorDomainId);
        domainMap.get(e.rawTargetDomainId);
    }

    const size_t nDomains      = domainMap.count();
    const size_t nClockDomains = clockMap.count();
    const size_t cpuTableSize  = maxCpuId + 1;

    // -------------------------------------------------------------------------
    // Construct the topology object with empty containers.
    // -------------------------------------------------------------------------
    NUMATopology topo;

    for (size_t i = 0; i < nDomains; i++) {
        ProximityDomain d;
        d.id = DomainID(static_cast<uint16_t>(i));
        topo.proximityDomains.push(move(d));
    }
    for (size_t i = 0; i < nClockDomains; i++) {
        ClockDomain cd;
        cd.id = ClockDomainID(static_cast<uint16_t>(i));
        topo.clockDomains.push(move(cd));
    }
    // Per-CPU lookup tables: pre-fill with null values.
    for (size_t i = 0; i < cpuTableSize; i++) {
        topo.cpuToDomain.push(DomainID{});
        topo.cpuToClockDomain.push(ClockDomainID{});
    }

    // -------------------------------------------------------------------------
    // Pass 2 — populate with duplicate detection.
    // -------------------------------------------------------------------------
    bool cpuSeen[arch::MAX_PROCESSOR_COUNT] = {};

    // Per-clock-domain set of proximity domain IDs already registered.
    Vector<HashSet<uint16_t>> clockProxSeen;
    for (size_t i = 0; i < nClockDomains; i++)
        clockProxSeen.push(HashSet<uint16_t>{});

    for (const auto& e : procs) {
        const size_t cpuIdx = static_cast<size_t>(e.cpuId);
        if (cpuSeen[cpuIdx]) {
            klog() << "[NUMA] Warning: duplicate SRAT processor entry for CPU "
                   << static_cast<uint32_t>(e.cpuId) << ", ignoring\n";
            continue;
        }
        cpuSeen[cpuIdx] = true;

        const DomainID      did = domainMap.get(e.rawDomainId);
        const ClockDomainID cid = clockMap.get(e.rawClockDomainId);

        topo.proximityDomains[static_cast<size_t>(did.value)].processors.push(e.cpuId);
        topo.clockDomains[static_cast<size_t>(cid.value)].cpus.push(e.cpuId);

        // Register proximity domain in its clock domain (deduplicated via HashSet).
        auto& seen = clockProxSeen[static_cast<size_t>(cid.value)];
        if (!seen.contains(did.value)) {
            seen.insert(did.value);
            topo.clockDomains[static_cast<size_t>(cid.value)].proximityDomains.push(did);
        }

        topo.cpuToDomain[cpuIdx]      = did;
        topo.cpuToClockDomain[cpuIdx] = cid;
    }

    for (const auto& e : mems) {
        const DomainID did = domainMap.get(e.rawDomainId);
        auto& regions = topo.proximityDomains[static_cast<size_t>(did.value)].memoryRegions;
        bool duplicate = false;
        for (const auto& r : regions) {
            if (r.range == e.range) {
                klog() << "[NUMA] Warning: duplicate memory range in SRAT, ignoring\n";
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            regions.push({e.range, e.memType});
    }

    for (const auto& e : gens) {
        const DomainID did = domainMap.get(e.rawDomainId);
        topo.proximityDomains[static_cast<size_t>(did.value)].genericInitiators.push(e.device);
    }

    // -------------------------------------------------------------------------
    // Compute domain role flags.
    // -------------------------------------------------------------------------
    for (auto& dom : topo.proximityDomains) {
        dom.isInitiatorDomain = !dom.processors.empty() || !dom.genericInitiators.empty();
        dom.isTargetDomain    = !dom.memoryRegions.empty();
    }

    // -------------------------------------------------------------------------
    // Compute preferred initiator domain for memory-only domains.
    // When exactly one initiator domain exists, assign it as preferred for all
    // memory-only target domains.
    // -------------------------------------------------------------------------
    size_t   initiatorCount = 0;
    DomainID soleInitiator  = DomainID::null();
    for (const auto& dom : topo.proximityDomains)
        if (dom.isInitiatorDomain) { initiatorCount++; soleInitiator = dom.id; }

    if (initiatorCount == 1) {
        for (auto& dom : topo.proximityDomains) {
            if (dom.isTargetDomain && !dom.isInitiatorDomain)
                dom.preferredInitiatorDomain = soleInitiator;
        }
    }

    // -------------------------------------------------------------------------
    // Validate CPU coverage: ensure every CPU known to the arch layer has a
    // NUMA domain entry, inserting a dummy (domain 0) assignment for any
    // omitted by firmware.
    // -------------------------------------------------------------------------
    const size_t totalCpus = arch::processorCount();
    if (topo.cpuToDomain.size() < totalCpus) {
        while (topo.cpuToDomain.size() < totalCpus) {
            topo.cpuToDomain.push(DomainID{});
            topo.cpuToClockDomain.push(ClockDomainID{});
        }
    }
    for (size_t i = 0; i < totalCpus; i++) {
        if (topo.cpuToDomain[i] == DomainID::null()) {
            klog() << "[NUMA] Warning: CPU " << static_cast<uint32_t>(i)
                   << " has no SRAT entry; assigning to domain 0\n";
            topo.cpuToDomain[i]      = DomainID(0);
            topo.cpuToClockDomain[i] = ClockDomainID(0);
            topo.proximityDomains[0].processors.push(static_cast<arch::ProcessorID>(i));
            topo.proximityDomains[0].isInitiatorDomain = true;
        }
    }

    // -------------------------------------------------------------------------
    // Latency matrices (separate read and write).
    // Cells are initialised to DISTANCE_NO_DATA.
    // If both HMAT (absolute) and SLIT (relative) entries are present, prefer HMAT.
    // -------------------------------------------------------------------------
    bool hasAbsoluteEntries = false;
    bool hasRelativeEntries = false;
    for (const auto& e : lats) {
        if (e.absolute) hasAbsoluteEntries = true;
        else            hasRelativeEntries = true;
    }

    if (!lats.empty()) {
        if (hasAbsoluteEntries && hasRelativeEntries)
            klog() << "[NUMA] Warning: mixed HMAT/SLIT latency entries — using HMAT values\n";

        const bool useAbsolute = hasAbsoluteEntries;
        bool hasReadEntries = false, hasWriteEntries = false;
        for (const auto& e : lats) {
            if (e.absolute != useAbsolute) continue;
            if (e.readLatency  != DISTANCE_NO_DATA) hasReadEntries  = true;
            if (e.writeLatency != DISTANCE_NO_DATA) hasWriteEntries = true;
        }

        if (hasReadEntries)
            topo.readLatencyMatrix = DArray<uint64_t, 2>(
                [](size_t, size_t) { return DISTANCE_NO_DATA; }, nDomains, nDomains);
        if (hasWriteEntries)
            topo.writeLatencyMatrix = DArray<uint64_t, 2>(
                [](size_t, size_t) { return DISTANCE_NO_DATA; }, nDomains, nDomains);

        for (const auto& e : lats) {
            if (e.absolute != useAbsolute) continue;
            const DomainID init = domainMap.get(e.rawInitiatorDomainId);
            const DomainID tgt  = domainMap.get(e.rawTargetDomainId);
            if (e.readLatency != DISTANCE_NO_DATA) {
                auto& cell = (*topo.readLatencyMatrix)[init.value, tgt.value];
                if (cell != DISTANCE_NO_DATA) {
                    if (cell != e.readLatency)
                        klog() << "[NUMA] Warning: conflicting read latency entries, keeping first\n";
                } else {
                    cell = e.readLatency;
                }
            }
            if (e.writeLatency != DISTANCE_NO_DATA) {
                auto& cell = (*topo.writeLatencyMatrix)[init.value, tgt.value];
                if (cell != DISTANCE_NO_DATA) {
                    if (cell != e.writeLatency)
                        klog() << "[NUMA] Warning: conflicting write latency entries, keeping first\n";
                } else {
                    cell = e.writeLatency;
                }
            }
        }

        topo.quality.latency = useAbsolute ? DataQualityLevel::Absolute
                                           : DataQualityLevel::Relative;
    }

    // -------------------------------------------------------------------------
    // Bandwidth matrices (separate read and write).
    // HMAT bandwidth values are always absolute (MB/s).
    // -------------------------------------------------------------------------
    if (!bws.empty()) {
        bool hasReadBw = false, hasWriteBw = false;
        for (const auto& e : bws) {
            if (e.readBandwidth  != DISTANCE_NO_DATA) hasReadBw  = true;
            if (e.writeBandwidth != DISTANCE_NO_DATA) hasWriteBw = true;
        }

        if (hasReadBw)
            topo.readBandwidthMatrix = DArray<uint64_t, 2>(
                [](size_t, size_t) { return DISTANCE_NO_DATA; }, nDomains, nDomains);
        if (hasWriteBw)
            topo.writeBandwidthMatrix = DArray<uint64_t, 2>(
                [](size_t, size_t) { return DISTANCE_NO_DATA; }, nDomains, nDomains);

        for (const auto& e : bws) {
            const DomainID init = domainMap.get(e.rawInitiatorDomainId);
            const DomainID tgt  = domainMap.get(e.rawTargetDomainId);
            if (e.readBandwidth != DISTANCE_NO_DATA) {
                auto& cell = (*topo.readBandwidthMatrix)[init.value, tgt.value];
                if (cell != DISTANCE_NO_DATA) {
                    if (cell != e.readBandwidth)
                        klog() << "[NUMA] Warning: conflicting read bandwidth entries, keeping first\n";
                } else {
                    cell = e.readBandwidth;
                }
            }
            if (e.writeBandwidth != DISTANCE_NO_DATA) {
                auto& cell = (*topo.writeBandwidthMatrix)[init.value, tgt.value];
                if (cell != DISTANCE_NO_DATA) {
                    if (cell != e.writeBandwidth)
                        klog() << "[NUMA] Warning: conflicting write bandwidth entries, keeping first\n";
                } else {
                    cell = e.writeBandwidth;
                }
            }
        }

        topo.quality.bandwidth = DataQualityLevel::Absolute;
    }

    // -------------------------------------------------------------------------
    // Cache info placeholder (one slot per domain, all unset at this layer).
    // HMAT memory-side cache structures can be used to populate these later.
    // -------------------------------------------------------------------------
    for (size_t i = 0; i < nDomains; i++)
        topo.cacheInfo.push(Optional<DomainCacheInfo>{});

    const auto qualStr = [](DataQualityLevel q) {
        return q == DataQualityLevel::Absolute ? "Absolute"
             : q == DataQualityLevel::Relative ? "Relative"
                                               : "Inferred";
    };
    klog() << "[NUMA] Topology built: "
           << static_cast<uint32_t>(nDomains) << " proximity domains, "
           << static_cast<uint32_t>(nClockDomains) << " clock domains, "
           << "latency=" << qualStr(topo.quality.latency)
           << " bandwidth=" << qualStr(topo.quality.bandwidth)
           << '\n';
    return topo;
}

// ============================================================================
// NUMATopology — membership queries
// ============================================================================

const ProximityDomain& NUMATopology::domainForCpu(arch::ProcessorID cpu) const {
    const size_t idx = static_cast<size_t>(cpu);
    assert(idx < cpuToDomain.size(), "NUMATopology::domainForCpu: CPU ID out of range");
    const DomainID did = cpuToDomain[idx];
    assert(did != DomainID::null(), "NUMATopology::domainForCpu: CPU has no NUMA domain");
    return proximityDomains[static_cast<size_t>(did.value)];
}

const ProximityDomain* NUMATopology::domainForAddress(mm::phys_addr addr) const {
    for (const auto& dom : proximityDomains) {
        for (const auto& region : dom.memoryRegions) {
            if (region.range.contains(addr)) return &dom;
        }
    }
    return nullptr;
}

const ProximityDomain* NUMATopology::domainForDevice(PciSBDF sbdf) const {
    for (const auto& dom : proximityDomains) {
        for (const auto& dev : dom.genericInitiators) {
            if (dev.holds<PciSBDF>() && dev.get<PciSBDF>() == sbdf)
                return &dom;
        }
    }
    return nullptr;
}

const ProximityDomain& NUMATopology::domain(DomainID id) const {
    assert(id != DomainID::null() && static_cast<size_t>(id.value) < proximityDomains.size(),
           "NUMATopology::domain: invalid domain ID");
    return proximityDomains[static_cast<size_t>(id.value)];
}

const ClockDomain& NUMATopology::clockDomainForCpu(arch::ProcessorID cpu) const {
    const size_t idx = static_cast<size_t>(cpu);
    assert(idx < cpuToClockDomain.size(), "NUMATopology::clockDomainForCpu: CPU ID out of range");
    const ClockDomainID cid = cpuToClockDomain[idx];
    assert(cid != ClockDomainID::null(), "NUMATopology::clockDomainForCpu: CPU has no clock domain");
    return clockDomains[static_cast<size_t>(cid.value)];
}

bool NUMATopology::shareClockDomain(arch::ProcessorID a, arch::ProcessorID b) const {
    const size_t ai = static_cast<size_t>(a);
    const size_t bi = static_cast<size_t>(b);
    assert(ai < cpuToClockDomain.size() && bi < cpuToClockDomain.size(),
           "NUMATopology::shareClockDomain: CPU ID out of range");
    return cpuToClockDomain[ai] == cpuToClockDomain[bi];
}

size_t NUMATopology::cpuCount() const {
    return cpuToDomain.size();
}

// ============================================================================
// NUMATopology — iteration
// ============================================================================

IteratorRange<const ProximityDomain*> NUMATopology::domains() const {
    return { proximityDomains.begin(), proximityDomains.end() };
}

FilteredIterator<IsInitiatorDomain, const ProximityDomain*> NUMATopology::initiatorDomains() const {
    return { proximityDomains.begin(), proximityDomains.end(), IsInitiatorDomain{} };
}

FilteredIterator<IsTargetDomain, const ProximityDomain*> NUMATopology::targetDomains() const {
    return { proximityDomains.begin(), proximityDomains.end(), IsTargetDomain{} };
}

NUMATopology::AllMemoryRegionsRange NUMATopology::allMemoryRegions() const {
    return { proximityDomains.begin(), proximityDomains.end() };
}

IteratorRange<const arch::ProcessorID*> NUMATopology::cpusInDomain(DomainID id) const {
    const auto& dom = domain(id);
    return { dom.processors.begin(), dom.processors.end() };
}

IteratorRange<const MemoryRegion*> NUMATopology::memoryRegionsInDomain(DomainID id) const {
    const auto& dom = domain(id);
    return { dom.memoryRegions.begin(), dom.memoryRegions.end() };
}

IteratorRange<const DeviceHandle*> NUMATopology::devicesInDomain(DomainID id) const {
    const auto& dom = domain(id);
    return { dom.genericInitiators.begin(), dom.genericInitiators.end() };
}

// ============================================================================
// NUMATopology — distance / ordering queries
// ============================================================================

Vector<DomainDistance> NUMATopology::domainsOrdered(
    DomainID initiator, DistanceMetric metric) const
{
    assert(initiator != DomainID::null() &&
           static_cast<size_t>(initiator.value) < proximityDomains.size(),
           "NUMATopology::domainsOrdered: invalid initiator domain");

    const size_t n = proximityDomains.size();
    Vector<DomainDistance> result(n);

    // Try bandwidth ordering if requested and available.
    if ((metric == DistanceMetric::Bandwidth || metric == DistanceMetric::Balanced)
        && readBandwidthMatrix.occupied())
    {
        for (size_t j = 0; j < n; j++) {
            const uint64_t bw = (*readBandwidthMatrix)[initiator.value, static_cast<uint16_t>(j)];
            result.push({DomainID(static_cast<uint16_t>(j)), bw, quality.bandwidth});
        }
        // Bandwidth: highest first (descending). Sentinels (NO_DATA, UNREACHABLE)
        // sort after valid values; NO_DATA before UNREACHABLE.
        result.sort([](const DomainDistance& a, const DomainDistance& b) {
            const bool aSentinel = a.distance >= DISTANCE_NO_DATA;
            const bool bSentinel = b.distance >= DISTANCE_NO_DATA;
            if (aSentinel != bSentinel) return !aSentinel; // valid before sentinel
            if (aSentinel) return a.distance < b.distance; // NO_DATA before UNREACHABLE
            return a.distance > b.distance;                // descending by bandwidth
        });
        return result;
    }

    // Try latency ordering if available.
    // Sentinels sort naturally to the end: valid values < NO_DATA < UNREACHABLE.
    if (readLatencyMatrix.occupied()) {
        for (size_t j = 0; j < n; j++) {
            const uint64_t lat = (*readLatencyMatrix)[initiator.value, static_cast<uint16_t>(j)];
            result.push({DomainID(static_cast<uint16_t>(j)), lat, quality.latency});
        }
        // Latency: lowest first (ascending).
        result.sort([](const DomainDistance& a, const DomainDistance& b) {
            return a.distance < b.distance;
        });
        return result;
    }

    // Inferred fallback: local domain first, all others equal distance.
    for (size_t j = 0; j < n; j++) {
        const uint64_t dist = (j == static_cast<size_t>(initiator.value)) ? 0 : 1;
        result.push({DomainID(static_cast<uint16_t>(j)), dist, DataQualityLevel::Inferred});
    }
    result.sort([](const DomainDistance& a, const DomainDistance& b) {
        return a.distance < b.distance;
    });
    return result;
}

Vector<DomainDistance> NUMATopology::domainsOrderedByLatency(DomainID initiator) const {
    return domainsOrdered(initiator, DistanceMetric::Latency);
}

Vector<DomainDistance> NUMATopology::domainsOrderedByBandwidth(DomainID initiator) const {
    return domainsOrdered(initiator, DistanceMetric::Bandwidth);
}

// ============================================================================
// NUMATopology — raw attribute access
// ============================================================================

Optional<uint64_t> NUMATopology::latencyBetween(DomainID from, DomainID to,
                                                  AccessDirection dir) const {
    const auto& matrix = (dir == AccessDirection::Write) ? writeLatencyMatrix
                                                          : readLatencyMatrix;
    if (!matrix.occupied()) return Optional<uint64_t>{};
    const uint64_t v = (*matrix)[from.value, to.value];
    if (v >= DISTANCE_NO_DATA) return Optional<uint64_t>{};
    return Optional<uint64_t>{v};
}

Optional<uint64_t> NUMATopology::bandwidthBetween(DomainID from, DomainID to,
                                                    AccessDirection dir) const {
    const auto& matrix = (dir == AccessDirection::Write) ? writeBandwidthMatrix
                                                          : readBandwidthMatrix;
    if (!matrix.occupied()) return Optional<uint64_t>{};
    const uint64_t v = (*matrix)[from.value, to.value];
    if (v >= DISTANCE_NO_DATA) return Optional<uint64_t>{};
    return Optional<uint64_t>{v};
}

DataQuality NUMATopology::dataQuality() const { return quality; }

Optional<DomainCacheInfo> NUMATopology::cacheInfoForDomain(DomainID id) const {
    assert(id != DomainID::null() && static_cast<size_t>(id.value) < cacheInfo.size(),
           "NUMATopology::cacheInfoForDomain: invalid domain ID");
    return cacheInfo[static_cast<size_t>(id.value)];
}

size_t NUMATopology::domainCount() const { return proximityDomains.size(); }

// ============================================================================
// NUMATopology — topology flags
// ============================================================================

bool NUMATopology::isTrivial()              const { return proximityDomains.size() <= 1; }
bool NUMATopology::hasLatencyInfo()         const { return readLatencyMatrix.occupied() || writeLatencyMatrix.occupied(); }
bool NUMATopology::hasAbsoluteLatencyInfo() const { return quality.latency == DataQualityLevel::Absolute; }
bool NUMATopology::hasBandwidthInfo()       const { return readBandwidthMatrix.occupied() || writeBandwidthMatrix.occupied(); }

// ============================================================================
// NUMAPolicy — construction / destruction
// ============================================================================

NUMAPolicy::NUMAPolicy(const NUMATopology& topology) {
    cpuCount    = topology.cpuCount();
    domainCount = topology.domainCount();
    quality     = topology.dataQuality().latency;

    // Allocate and fill per-CPU home-domain and clock-domain arrays.
    if (cpuCount > 0) {
        cpuHomeDomain  = static_cast<DomainID*>(
            operator new(sizeof(DomainID) * cpuCount));
        cpuClockDomain = static_cast<ClockDomainID*>(
            operator new(sizeof(ClockDomainID) * cpuCount));

        for (size_t i = 0; i < cpuCount; i++) {
            const arch::ProcessorID cpu = static_cast<arch::ProcessorID>(i);
            // Use domain 0 as fallback for CPUs not listed in SRAT
            // (shouldn't happen on correct firmware, but is graceful).
            const size_t cpuIdx = static_cast<size_t>(cpu);
            if (cpuIdx < topology.cpuToDomain.size() &&
                topology.cpuToDomain[cpuIdx] != DomainID::null())
            {
                cpuHomeDomain[i]  = topology.cpuToDomain[cpuIdx];
                cpuClockDomain[i] = topology.cpuToClockDomain[cpuIdx];
            } else {
                cpuHomeDomain[i]  = DomainID(0);
                cpuClockDomain[i] = ClockDomainID(0);
            }
        }
    }

    // Precompute the fallback ordering table.
    if (domainCount > 0) {
        fallbackOrder = static_cast<DomainID*>(
            operator new(sizeof(DomainID) * domainCount * domainCount));

        for (size_t i = 0; i < domainCount; i++) {
            const auto ordered = topology.domainsOrdered(
                DomainID(static_cast<uint16_t>(i)), DistanceMetric::Latency);
            for (size_t j = 0; j < domainCount; j++) {
                fallbackOrder[i * domainCount + j] =
                    (j < ordered.size())
                        ? ordered[j].domainId
                        : DomainID(static_cast<uint16_t>(j)); // identity fallback
            }
        }
    }
}

NUMAPolicy::NUMAPolicy(NUMAPolicy&& other) noexcept
    : cpuHomeDomain(other.cpuHomeDomain),
      cpuClockDomain(other.cpuClockDomain),
      fallbackOrder(other.fallbackOrder),
      cpuCount(other.cpuCount),
      domainCount(other.domainCount),
      quality(other.quality)
{
    other.cpuHomeDomain  = nullptr;
    other.cpuClockDomain = nullptr;
    other.fallbackOrder  = nullptr;
}

NUMAPolicy& NUMAPolicy::operator=(NUMAPolicy&& other) noexcept {
    if (this == &other) return *this;
    operator delete(cpuHomeDomain);
    operator delete(cpuClockDomain);
    operator delete(fallbackOrder);
    cpuHomeDomain  = other.cpuHomeDomain;
    cpuClockDomain = other.cpuClockDomain;
    fallbackOrder  = other.fallbackOrder;
    cpuCount       = other.cpuCount;
    domainCount    = other.domainCount;
    quality        = other.quality;
    other.cpuHomeDomain  = nullptr;
    other.cpuClockDomain = nullptr;
    other.fallbackOrder  = nullptr;
    return *this;
}

NUMAPolicy::~NUMAPolicy() {
    if (cpuHomeDomain)  operator delete(cpuHomeDomain);
    if (cpuClockDomain) operator delete(cpuClockDomain);
    if (fallbackOrder)  operator delete(fallbackOrder);
}

// ============================================================================
// NUMAPolicy — hot-path queries
// ============================================================================

DomainID NUMAPolicy::homeDomain(arch::ProcessorID cpu) const {
    assert(static_cast<size_t>(cpu) < cpuCount, "NUMAPolicy::homeDomain: CPU ID out of range");
    return cpuHomeDomain[static_cast<size_t>(cpu)];
}

ClockDomainID NUMAPolicy::clockDomain(arch::ProcessorID cpu) const {
    assert(static_cast<size_t>(cpu) < cpuCount, "NUMAPolicy::clockDomain: CPU ID out of range");
    return cpuClockDomain[static_cast<size_t>(cpu)];
}

Span<const DomainID> NUMAPolicy::domainOrder(DomainID initiator) const {
    assert(initiator != DomainID::null() &&
           static_cast<size_t>(initiator.value) < domainCount,
           "NUMAPolicy::domainOrder: invalid domain ID");
    return { fallbackOrder + static_cast<size_t>(initiator.value) * domainCount,
             domainCount };
}

bool NUMAPolicy::shareClockDomain(arch::ProcessorID a, arch::ProcessorID b) const {
    assert(static_cast<size_t>(a) < cpuCount && static_cast<size_t>(b) < cpuCount,
           "NUMAPolicy::shareClockDomain: CPU ID out of range");
    return cpuClockDomain[static_cast<size_t>(a)] ==
           cpuClockDomain[static_cast<size_t>(b)];
}

DataQualityLevel NUMAPolicy::orderingQuality() const { return quality; }

// ============================================================================
// Global policy singleton
// ============================================================================

// ============================================================================
// partitionMemoryByDomain
// ============================================================================

static constexpr size_t kMinPartitionRangeSize = arch::smallPageSize;

NUMAMemoryPartition partitionMemoryByDomain(
    const Vector<mm::phys_memory_range>& usableRanges,
    size_t processorCount,
    const NUMATopology& topology,
    DistanceMetric metric)
{
    NUMAMemoryPartition result;
    const size_t domainCount = topology.domainCount();

    for (size_t d = 0; d < domainCount; d++)
        result.rangesPerDomain.push(Vector<mm::phys_memory_range>{});

    // ---------------------------------------------------------------
    // Step 1: walk usable memory ranges, split along small-page-aligned
    // NUMA domain boundaries.
    // ---------------------------------------------------------------
    for (const auto& entry : usableRanges) {
        const uint64_t alignedStart = roundUpToNearestMultiple(
            entry.start.value, static_cast<uint64_t>(arch::smallPageSize));
        const uint64_t alignedEnd   = roundDownToNearestMultiple(
            entry.end.value,   static_cast<uint64_t>(arch::smallPageSize));

        if (alignedEnd <= alignedStart) continue;

        // Build a sorted list of small-page-aligned split points by projecting
        // each NUMA region boundary into this usable range.  The count is
        // O(domains × regions_per_domain) — far smaller than smallPages.
        Vector<uint64_t> splits;
        splits.push(alignedStart);
        splits.push(alignedEnd);

        for (const auto& mr : topology.allMemoryRegions()) {
            const uint64_t rStart = roundUpToNearestMultiple(
                mr.region.range.start.value, static_cast<uint64_t>(arch::smallPageSize));
            const uint64_t rEnd   = roundDownToNearestMultiple(
                mr.region.range.end.value,   static_cast<uint64_t>(arch::smallPageSize));

            if (rStart > alignedStart && rStart < alignedEnd)
                splits.push(rStart);
            if (rEnd > alignedStart && rEnd < alignedEnd)
                splits.push(rEnd);
        }

        // Insertion sort — count is tiny (2 + 2 × NUMA region count).
        for (size_t s = 1; s < splits.size(); s++) {
            const uint64_t key = splits[s];
            size_t j = s;
            while (j > 0 && splits[j - 1] > key) {
                splits[j] = splits[j - 1];
                j--;
            }
            splits[j] = key;
        }

        // Walk intervals between consecutive split points, calling
        // domainForAddress once per interval instead of once per big page.
        const ProximityDomain* runDomain = nullptr;
        mm::phys_addr runStart{alignedStart};

        auto flushRun = [&](mm::phys_addr flushEnd) {
            mm::phys_memory_range r{runStart, flushEnd};
            if (r.getSize() < kMinPartitionRangeSize) return;
            if (runDomain) {
                result.rangesPerDomain[runDomain->id.value].push(r);
            } else {
                klog() << "[NUMA] Warning: usable range [0x" << (void*)runStart.value
                       << ", 0x" << (void*)flushEnd.value
                       << ") has no NUMA domain affinity\n";
                result.unownedRanges.push(r);
            }
        };

        for (size_t s = 0; s + 1 < splits.size(); s++) {
            if (splits[s] == splits[s + 1]) continue; // skip duplicates

            const mm::phys_addr intervalStart{splits[s]};
            const mm::phys_addr intervalEnd{splits[s + 1]};

            const ProximityDomain* intervalDomain =
                topology.domainForAddress(intervalStart);

            // Straddle check: our split points snap NUMA boundaries to big-page
            // alignment, so an interval can still contain a domain boundary if the
            // original boundary was not big-page aligned.
            const ProximityDomain* tailDomain =
                topology.domainForAddress(mm::phys_addr{intervalEnd.value - 1});
            if (intervalDomain != tailDomain) {
                klog() << "[NUMA] Warning: interval [0x" << (void*)intervalStart.value
                       << ", 0x" << (void*)intervalEnd.value
                       << ") straddles a NUMA domain boundary; "
                       << "assigning to start-address domain\n";
                // intervalDomain (start) wins
            }

            if (intervalDomain != runDomain) {
                if (intervalStart.value > alignedStart)
                    flushRun(intervalStart);
                runDomain = intervalDomain;
                runStart  = intervalStart;
            }
        }
        flushRun(mm::phys_addr{alignedEnd});
    }

    // ---------------------------------------------------------------
    // Step 2: for each ProcessorID, walk the distance-ordered domain
    // list and pick the first domain that has at least one range.
    // ---------------------------------------------------------------

    // Identify a last-resort fallback (first domain with any ranges).
    DomainID fallback = DomainID::null();
    for (size_t d = 0; d < domainCount; d++) {
        if (!result.rangesPerDomain[d].empty()) {
            fallback = DomainID{static_cast<uint16_t>(d)};
            break;
        }
    }

    for (size_t i = 0; i < processorCount; i++) {
        const arch::ProcessorID cpu = static_cast<arch::ProcessorID>(i);
        const DomainID homeDomain = topology.domainForCpu(cpu).id;

        DomainID chosen = DomainID::null();
        for (const auto& dd : topology.domainsOrdered(homeDomain, metric)) {
            if (!result.rangesPerDomain[dd.domainId.value].empty()) {
                chosen = dd.domainId;
                break;
            }
        }

        if (chosen == DomainID::null()) {
            klog() << "[NUMA] Warning: CPU " << static_cast<uint32_t>(cpu)
                   << " has no reachable domain with memory; "
                   << "assigning to domain " << fallback.value << "\n";
            chosen = fallback;
        } else if (chosen != homeDomain) {
            klog() << "[NUMA] Warning: CPU " << static_cast<uint32_t>(cpu)
                   << " home domain " << homeDomain.value
                   << " has no memory; nearest domain with memory is "
                   << chosen.value << "\n";
        }

        result.processorDomain.push(chosen);
    }

    return result;
}

// ============================================================================

static NUMAPolicy* gNUMAPolicy = nullptr;

void initNUMAPolicy(const NUMATopology& topology) {
    assert(gNUMAPolicy == nullptr, "initNUMAPolicy called more than once");
    gNUMAPolicy = new NUMAPolicy(topology);
    klog() << "[NUMA] Policy initialised: "
                   << static_cast<uint32_t>(topology.domainCount()) << " domains\n";
}

const NUMAPolicy& numaPolicy() {
    assert(gNUMAPolicy != nullptr, "numaPolicy() called before initNUMAPolicy()");
    return *gNUMAPolicy;
}

} // namespace kernel::numa
