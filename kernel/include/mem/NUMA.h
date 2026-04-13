//
// Created by Spencer Martin on 3/28/26.
//
// NUMA topology and policy abstractions.
//
// NUMATopology is the rich boot-time structure built from hardware-description
// iterators (SRAT, HMAT, SLIT, CEDT).  It is constructed once and never mutated.
//
// NUMAPolicy is a lightweight derivative optimised for hot-path queries.  It is
// derived from NUMATopology and stored in a global.
//
// Neither structure parses ACPI tables directly — on x86-64, they consume typed
// iterator ranges produced by the iterator layer in acpi/NUMAIterators.h.
//

#ifndef CROCOS_NUMA_H
#define CROCOS_NUMA_H

#include <stdint.h>
#include <stddef.h>
#include <arch.h>
#include <mem/MemTypes.h>
#include <core/ds/Vector.h>
#include <core/ds/Optional.h>
#include <core/ds/Variant.h>
#include <core/ds/Array.h>
#include <core/ds/Span.h>
#include <core/Iterator.h>

namespace kernel::numa {

// ============================================================================
// Strong ID types
// ============================================================================
// DomainID and ClockDomainID are both normalised to sequential integers from 0,
// but they inhabit distinct namespaces.  The type system makes them hard to
// confuse: DomainID(2) and ClockDomainID(2) are unrelated values.

struct DomainID {
    uint16_t value = UINT16_MAX;  // UINT16_MAX == null/invalid sentinel

    constexpr explicit DomainID(uint16_t v) : value(v) {}
    constexpr DomainID() = default;

    bool operator==(const DomainID& o) const { return value == o.value; }
    bool operator!=(const DomainID& o) const { return value != o.value; }

    // Nullable concept support — enables Optional<DomainID> = ImplicitOptional<DomainID>
    static DomainID null() { return DomainID{}; }
};

struct ClockDomainID {
    uint16_t value = UINT16_MAX;

    constexpr explicit ClockDomainID(uint16_t v) : value(v) {}
    constexpr ClockDomainID() = default;

    bool operator==(const ClockDomainID& o) const { return value == o.value; }
    bool operator!=(const ClockDomainID& o) const { return value != o.value; }

    static ClockDomainID null() { return ClockDomainID{}; }
};

// ============================================================================
// Enumerations
// ============================================================================

enum class MemoryType : uint8_t {
    Volatile,    // Standard DRAM
    Nonvolatile  // Persistent / NVDIMM memory
};

// Describes how trustworthy the distance / ordering information is for one metric.
enum class DataQualityLevel : uint8_t {
    Absolute,  // From HMAT — real picosecond / MB/s values
    Relative,  // From SLIT — normalised distances, useful for ordering only
    Inferred   // No table present; local-before-remote heuristic applied
};

// Quality of distance data reported independently for latency and bandwidth.
struct DataQuality {
    DataQualityLevel latency   = DataQualityLevel::Inferred;
    DataQualityLevel bandwidth = DataQualityLevel::Inferred;
};

// Sentinel values stored in latency / bandwidth matrices.
// DISTANCE_NO_DATA:     entry not provided by firmware (path may exist but was not measured).
// DISTANCE_UNREACHABLE: firmware explicitly reported no path between these domains.
// For ordering: valid values < NO_DATA < UNREACHABLE.
constexpr uint64_t DISTANCE_NO_DATA     = UINT64_MAX - 1;
constexpr uint64_t DISTANCE_UNREACHABLE = UINT64_MAX;

// Selects which metric to use for domain ordering queries.
enum class DistanceMetric : uint8_t {
    Latency,
    Bandwidth,
    Balanced   // Try bandwidth, fall back to latency, fall back to inferred
};

// Selects the access direction for latency and bandwidth queries.
enum class AccessDirection : uint8_t {
    Read,
    Write
};

// ============================================================================
// Device handle
// ============================================================================

struct PciSBDF {
    uint16_t segment;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;

    bool operator==(const PciSBDF& o) const {
        return segment == o.segment && bus == o.bus &&
               device == o.device && function == o.function;
    }
};

struct AcpiHandle {
    uint32_t value;
    bool operator==(const AcpiHandle& o) const { return value == o.value; }
};

// A device is identified either by PCI SBDF or by ACPI handle.
using DeviceHandle = Variant<PciSBDF, AcpiHandle>;

// ============================================================================
// Memory region
// ============================================================================

struct MemoryRegion {
    mm::phys_memory_range range;
    MemoryType            type;
};

// ============================================================================
// Input iterator entry types
// ============================================================================
// These are the "currency" exchanged between the ACPI iterator layer and
// NUMATopology::build().  Raw proximity domain IDs in these structs are
// normalised inside build() and must not leak upward.

struct ProcessorAffinityEntry {
    arch::ProcessorID cpuId;
    uint32_t          rawDomainId;
    uint32_t          rawClockDomainId;
};

struct MemoryRangeAffinityEntry {
    mm::phys_memory_range range;
    uint32_t              rawDomainId;
    MemoryType            memType;
};

struct GenericInitiatorEntry {
    DeviceHandle device;
    uint32_t     rawDomainId;
};

// Latency values are either absolute picoseconds (absolute == true) or
// SLIT-style normalised distances (absolute == false).
struct LatencyEntry {
    uint32_t rawInitiatorDomainId;
    uint32_t rawTargetDomainId;
    uint64_t readLatency  = DISTANCE_NO_DATA;  // DISTANCE_NO_DATA if direction not provided
    uint64_t writeLatency = DISTANCE_NO_DATA;
    bool     absolute;
};

struct BandwidthEntry {
    uint32_t rawInitiatorDomainId;
    uint32_t rawTargetDomainId;
    uint64_t readBandwidth  = DISTANCE_NO_DATA;  // DISTANCE_NO_DATA if direction not provided; MB/s otherwise
    uint64_t writeBandwidth = DISTANCE_NO_DATA;
};

// ============================================================================
// Output / result types
// ============================================================================

// One element in an ordered domain list returned by distance queries.
struct DomainDistance {
    DomainID         domainId;
    uint64_t         distance;  // Latency (ps or normalised) or bandwidth (MB/s)
    DataQualityLevel quality;   // How reliable this value is
};

// Yielded when iterating all memory regions across all domains.
struct MemoryRegionWithDomain {
    MemoryRegion region;
    DomainID     domainId;
};

// Present when a domain's memory acts as a hardware cache for another domain
// (e.g. HBM stacked on a DRAM domain).
struct DomainCacheInfo {
    DomainID cacheForDomain;      // The "backing" domain this caches
    uint32_t cacheLineSizeBytes;
    uint64_t totalCacheSizeBytes;
    uint8_t  associativity;
};

// ============================================================================
// EmptyIterable sentinel
// ============================================================================
// Pass EmptyIterable<LatencyEntry>{} to build() when no latency data is
// available.  The iterator's dereference operator is defined for type-checking
// purposes only and is never actually called.

template<typename T>
struct EmptyIterable {
    struct Iterator {
        bool operator!=(const Iterator&) const { return false; }
        Iterator& operator++() { return *this; }
        const T& operator*() const { __builtin_unreachable(); }
    };
    Iterator begin() const { return {}; }
    Iterator end()   const { return {}; }
};

// ============================================================================
// Core internal structures
// ============================================================================

struct ProximityDomain {
    DomainID                id;
    Vector<arch::ProcessorID>    processors;
    Vector<MemoryRegion>    memoryRegions;
    Vector<DeviceHandle>    genericInitiators;
    bool isInitiatorDomain = false; // true if contains CPUs or generic initiators
    bool isTargetDomain    = false; // true if contains memory regions

    // For memory-only domains: the "natural owner" initiator domain, if unambiguous.
    // Empty when equidistant from multiple initiators (e.g. CXL memory).
    // Note: full multi-initiator support (distance-weighted preferred initiator
    // assignment for CXL topologies) is future work.  Consumers should treat an
    // empty preferredInitiatorDomain as "no preference" and fall back to global
    // domain ordering, so that the per-domain side of the API does not need to
    // change when multi-initiator support is added.
    Optional<DomainID> preferredInitiatorDomain;
};

struct ClockDomain {
    ClockDomainID        id;
    Vector<arch::ProcessorID> cpus;
    Vector<DomainID>     proximityDomains; // which NUMA domains hold these CPUs
};

// NUMANode — variant unifying the three resource kinds inside a proximity domain.
// The per-type iteration API is preferred for most uses; NUMANode is provided for
// callers that want unified dispatch via Variant::visit().
struct ProcessorNode        { arch::ProcessorID cpuId;  };
struct MemoryRegionNode     { MemoryRegion       region; };
struct GenericInitiatorNode { DeviceHandle       device; };
using NUMANode = Variant<ProcessorNode, MemoryRegionNode, GenericInitiatorNode>;

// ============================================================================
// Domain filter predicates (for FilteredIterator)
// ============================================================================

struct IsInitiatorDomain {
    bool operator()(const ProximityDomain& d) const { return d.isInitiatorDomain; }
};

struct IsTargetDomain {
    bool operator()(const ProximityDomain& d) const { return d.isTargetDomain; }
};

// ============================================================================
// NUMATopology
// ============================================================================

class NUMATopology {
public:
    NUMATopology(const NUMATopology&)            = delete;
    NUMATopology& operator=(const NUMATopology&) = delete;
    NUMATopology(NUMATopology&&)                 = default;
    NUMATopology& operator=(NUMATopology&&)      = default;

    // ------------------------------------------------------------------
    // Factory
    // ------------------------------------------------------------------
    // build() accepts typed iterator ranges for each data category.  The latency
    // and bandwidth sources are independently optional — pass EmptyIterable<T>{}
    // when the corresponding ACPI table is absent.

    // Full form: all five categories.
    template<typename P, typename M, typename G, typename L, typename B>
    requires IterableWithValueType<P, ProcessorAffinityEntry>
          && IterableWithValueType<M, MemoryRangeAffinityEntry>
          && IterableWithValueType<G, GenericInitiatorEntry>
          && IterableWithValueType<L, LatencyEntry>
          && IterableWithValueType<B, BandwidthEntry>
    static NUMATopology build(P processors, M memory, G genericInitiators,
                               L latency, B bandwidth)
    {
        Vector<ProcessorAffinityEntry>   procVec;
        Vector<MemoryRangeAffinityEntry> memVec;
        Vector<GenericInitiatorEntry>    genVec;
        Vector<LatencyEntry>             latVec;
        Vector<BandwidthEntry>           bwVec;

        for (const auto& e : processors)        procVec.push(e);
        for (const auto& e : memory)            memVec.push(e);
        for (const auto& e : genericInitiators) genVec.push(e);
        for (const auto& e : latency)           latVec.push(e);
        for (const auto& e : bandwidth)         bwVec.push(e);

        return buildInternal(move(procVec), move(memVec), move(genVec),
                             move(latVec),  move(bwVec));
    }

    // Convenience: processors + memory + generic initiators + latency (no bandwidth).
    template<typename P, typename M, typename G, typename L>
    requires IterableWithValueType<P, ProcessorAffinityEntry>
          && IterableWithValueType<M, MemoryRangeAffinityEntry>
          && IterableWithValueType<G, GenericInitiatorEntry>
          && IterableWithValueType<L, LatencyEntry>
    static NUMATopology build(P processors, M memory, G genericInitiators, L latency) {
        return build(processors, memory, genericInitiators,
                     latency, EmptyIterable<BandwidthEntry>{});
    }

    // Convenience: processors + memory + generic initiators only.
    template<typename P, typename M, typename G>
    requires IterableWithValueType<P, ProcessorAffinityEntry>
          && IterableWithValueType<M, MemoryRangeAffinityEntry>
          && IterableWithValueType<G, GenericInitiatorEntry>
    static NUMATopology build(P processors, M memory, G genericInitiators) {
        return build(processors, memory, genericInitiators,
                     EmptyIterable<LatencyEntry>{}, EmptyIterable<BandwidthEntry>{});
    }

    // ------------------------------------------------------------------
    // Membership queries
    // ------------------------------------------------------------------

    // Returns the proximity domain for a given logical CPU.  Asserts on invalid ID.
    const ProximityDomain& domainForCpu(arch::ProcessorID cpu) const;

    // Returns the proximity domain containing addr, or nullptr if addr is outside
    // all known memory ranges.
    const ProximityDomain* domainForAddress(mm::phys_addr addr) const;

    // Returns the proximity domain for the given PCI device, or nullptr if none
    // was registered.
    const ProximityDomain* domainForDevice(PciSBDF sbdf) const;

    // Returns the proximity domain with the given normalised ID.
    // Asserts if id is out of range or null.
    const ProximityDomain& domain(DomainID id) const;

    // Returns the clock domain for a given logical CPU.  Asserts on invalid ID.
    const ClockDomain& clockDomainForCpu(arch::ProcessorID cpu) const;

    // Returns true iff two logical CPUs share a clock domain.
    bool shareClockDomain(arch::ProcessorID a, arch::ProcessorID b) const;

    // Number of CPUs that have been mapped to a proximity domain.
    size_t cpuCount() const;

    // ------------------------------------------------------------------
    // Iteration
    // ------------------------------------------------------------------

    // Range over all proximity domains.
    IteratorRange<const ProximityDomain*> domains() const;

    // Lazy range over initiator domains (those containing CPUs or generic initiators).
    FilteredIterator<IsInitiatorDomain, const ProximityDomain*> initiatorDomains() const;

    // Lazy range over memory / target domains.
    FilteredIterator<IsTargetDomain, const ProximityDomain*> targetDomains() const;

    // Flattened range over all memory regions across all domains, yielding
    // MemoryRegionWithDomain values.  Primary interface for the page allocator.
    struct AllMemoryRegionsRange {
        const ProximityDomain* domainsBegin;
        const ProximityDomain* domainsEnd;

        struct Iterator {
            const ProximityDomain* outer;
            const ProximityDomain* outerEnd;
            size_t                 inner;

            void skip() {
                while (outer != outerEnd && inner >= outer->memoryRegions.size()) {
                    ++outer;
                    inner = 0;
                }
            }
            Iterator(const ProximityDomain* o, const ProximityDomain* oe, size_t i)
                : outer(o), outerEnd(oe), inner(i) { skip(); }

            bool operator!=(const Iterator& o) const {
                return outer != o.outer || inner != o.inner;
            }
            MemoryRegionWithDomain operator*() const {
                return { outer->memoryRegions[inner], outer->id };
            }
            Iterator& operator++() { ++inner; skip(); return *this; }
        };

        Iterator begin() const { return {domainsBegin, domainsEnd, 0}; }
        Iterator end()   const { return {domainsEnd,   domainsEnd, 0}; }
    };
    AllMemoryRegionsRange allMemoryRegions() const;

    // Per-domain iteration helpers.
    IteratorRange<const arch::ProcessorID*> cpusInDomain(DomainID id) const;
    IteratorRange<const MemoryRegion*>      memoryRegionsInDomain(DomainID id) const;
    IteratorRange<const DeviceHandle*>      devicesInDomain(DomainID id) const;

    // ------------------------------------------------------------------
    // Distance / ordering queries
    // ------------------------------------------------------------------
    // Results carry a DistanceQuality tag so callers can judge reliability.
    // All three methods degrade gracefully: Absolute → Relative → Inferred.

    // Domains ordered by increasing latency (lowest / closest first).
    Vector<DomainDistance> domainsOrderedByLatency(DomainID initiator) const;

    // Domains ordered by decreasing bandwidth (highest first).
    // Falls back to latency ordering when bandwidth data is absent.
    Vector<DomainDistance> domainsOrderedByBandwidth(DomainID initiator) const;

    // General ordering using the specified metric, with graceful fallback.
    Vector<DomainDistance> domainsOrdered(DomainID initiator, DistanceMetric metric) const;

    // ------------------------------------------------------------------
    // Raw attribute access
    // ------------------------------------------------------------------

    // Latency in picoseconds (Absolute) or normalised units (Relative).
    // Returns empty if no latency data is available for the requested direction.
    Optional<uint64_t> latencyBetween(DomainID from, DomainID to,
                                       AccessDirection dir = AccessDirection::Read) const;

    // Bandwidth in MB/s.  Returns empty if no bandwidth data is available for
    // the requested direction.
    Optional<uint64_t> bandwidthBetween(DomainID from, DomainID to,
                                         AccessDirection dir = AccessDirection::Read) const;

    // Quality of distance data, reported independently for latency and bandwidth.
    DataQuality dataQuality() const;

    // Cache relationship for a domain (e.g. HBM acting as cache for DRAM domain).
    Optional<DomainCacheInfo> cacheInfoForDomain(DomainID id) const;

    // Total number of proximity domains.
    size_t domainCount() const;

    // ------------------------------------------------------------------
    // Topology flags
    // ------------------------------------------------------------------

    // True when there is only one proximity domain (common single-socket case).
    bool isTrivial()              const;
    bool hasLatencyInfo()         const;
    bool hasAbsoluteLatencyInfo() const;
    bool hasBandwidthInfo()       const;

private:
    NUMATopology() = default;

    friend class NUMAPolicy;  // NUMAPolicy reads per-CPU tables during construction.

    // Non-template core constructor — defined in NUMA.cpp.
    static NUMATopology buildInternal(
        Vector<ProcessorAffinityEntry>   procs,
        Vector<MemoryRangeAffinityEntry> mems,
        Vector<GenericInitiatorEntry>    gens,
        Vector<LatencyEntry>             lats,
        Vector<BandwidthEntry>           bws);

    Vector<ProximityDomain>           proximityDomains;
    Vector<ClockDomain>               clockDomains;

    // Per-CPU lookup tables, indexed by logical CPU ID.
    // Sized to accommodate the highest CPU ID seen during construction.
    Vector<DomainID>                  cpuToDomain;
    Vector<ClockDomainID>             cpuToClockDomain;

    // 2-D latency/bandwidth matrices indexed [initiatorDomain, targetDomain].
    // Each direction is tracked separately; absent when no data was provided for
    // that direction.  Cells are initialised to DISTANCE_NO_DATA.
    Optional<DArray<uint64_t, 2>>     readLatencyMatrix;
    Optional<DArray<uint64_t, 2>>     writeLatencyMatrix;
    Optional<DArray<uint64_t, 2>>     readBandwidthMatrix;
    Optional<DArray<uint64_t, 2>>     writeBandwidthMatrix;

    // Optional per-domain cache metadata (e.g. HBM-style hierarchies).
    Vector<Optional<DomainCacheInfo>> cacheInfo;

    DataQuality quality;
};

// ============================================================================
// NUMAPolicy
// ============================================================================
// Lightweight structure derived from NUMATopology at boot time.
// All fields are precomputed; every query is O(1).

class NUMAPolicy {
public:
    NUMAPolicy() = delete;
    NUMAPolicy(const NUMAPolicy&)            = delete;
    NUMAPolicy& operator=(const NUMAPolicy&) = delete;

    NUMAPolicy(NUMAPolicy&& other) noexcept;
    NUMAPolicy& operator=(NUMAPolicy&& other) noexcept;
    ~NUMAPolicy();

    explicit NUMAPolicy(const NUMATopology& topology);

    // Returns the home proximity domain for a logical CPU.  O(1).
    DomainID homeDomain(arch::ProcessorID cpu) const;

    // Returns the clock domain for a logical CPU.  O(1).
    ClockDomainID clockDomain(arch::ProcessorID cpu) const;

    // Returns a view over the fallback ordering for the given initiator domain.
    // domainOrder(d)[0] is the most preferred target domain.
    // The span is valid for the lifetime of this NUMAPolicy.  O(1).
    Span<const DomainID> domainOrder(DomainID initiator) const;

    // Returns true iff two logical CPUs share a clock domain.  O(1).
    bool shareClockDomain(arch::ProcessorID a, arch::ProcessorID b) const;

    // Quality level of the precomputed fallback ordering (latency-based).
    DataQualityLevel orderingQuality() const;

private:
    // Flat arrays allocated in the constructor; freed in the destructor.
    DomainID*        cpuHomeDomain  = nullptr; // [cpuCount]
    ClockDomainID*   cpuClockDomain = nullptr; // [cpuCount]
    DomainID*        fallbackOrder  = nullptr; // [domainCount * domainCount]
    size_t           cpuCount       = 0;
    size_t           domainCount    = 0;
    DataQualityLevel quality        = DataQualityLevel::Inferred;
};

// ============================================================================
// Global policy access
// ============================================================================

// Must be called exactly once, after NUMATopology is built and SMP is up.
void initNUMAPolicy(const NUMATopology& topology);

// Returns the global NUMAPolicy.  Asserts if not yet initialised.
const NUMAPolicy& numaPolicy();

// ============================================================================
// Memory partitioning
// ============================================================================

// Result of partitionMemoryByDomain().
//   rangesPerDomain[d.value] — usable big-page-aligned ranges belonging to domain d
//   unownedRanges            — usable ranges with no NUMA affinity
//   processorDomain[cpu]     — closest domain with memory for each logical CPU
struct NUMAMemoryPartition {
    Vector<Vector<mm::phys_memory_range>> rangesPerDomain; // indexed by DomainID.value
    Vector<mm::phys_memory_range>         unownedRanges;
    Vector<DomainID>                      processorDomain; // indexed by ProcessorID
};

// Splits usableRanges by NUMA proximity domain, aligning every sub-range to
// arch::smallPageSize boundaries and discarding sub-ranges smaller than
// arch::smallPageSize.
//
// processorCount determines the size of processorDomain in the result.
// All processor IDs [0, processorCount) receive a valid domain assignment;
// processors absent from the topology are assigned domain 0 (with a warning).
//
// Also produces a per-processor mapping to the closest domain (by `metric`)
// that actually contains memory map entries.
NUMAMemoryPartition partitionMemoryByDomain(
    const Vector<mm::phys_memory_range>& usableRanges,
    size_t processorCount,
    const NUMATopology& topology,
    DistanceMetric metric = DistanceMetric::Latency);

} // namespace kernel::numa

#endif // CROCOS_NUMA_H
