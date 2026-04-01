//
// Created by Spencer Martin on 3/28/26.
//
// ACPI SRAT / SLIT / HMAT iterator types for NUMATopology construction.
//
// These types live in the architecture-specific layer.  They read SRAT, SLIT,
// HMAT and CEDT and produce the typed entry values consumed by
// NUMATopology::build().  LAPIC-to-logical-CPU translation happens here.
//
// Usage:
//   auto topology = kernel::acpi::numa::buildNUMATopology();
//

#ifndef CROCOS_NUMA_ITERATORS_H
#define CROCOS_NUMA_ITERATORS_H

#include <acpi.h>
#include <mem/NUMA.h>
#include <arch/amd64/smp.h>

// ============================================================================
// ACPISignature specialisations for NUMA-related ACPI tables
// ============================================================================

namespace kernel::acpi {
struct SRAT; struct SLIT; struct HMAT; struct CEDT;

template <> struct ACPISignature<SRAT> { static constexpr const char* value = "SRAT"; };
template <> struct ACPISignature<SLIT> { static constexpr const char* value = "SLIT"; };
template <> struct ACPISignature<HMAT> { static constexpr const char* value = "HMAT"; };
template <> struct ACPISignature<CEDT> { static constexpr const char* value = "CEDT"; };
} // namespace kernel::acpi

// ============================================================================
// Packed ACPI table structures
// ============================================================================

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

namespace kernel::acpi {

// ---- SRAT subtable types ----
enum SRATSubtableType : uint8_t {
    SRAT_PROCESSOR_LAPIC     = 0,
    SRAT_MEMORY              = 1,
    SRAT_PROCESSOR_X2APIC    = 2,
    SRAT_GENERIC_INITIATOR   = 5
};

struct SRATSubtableHeader {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

// Type 0: Processor Local APIC / SAPIC affinity.
// The proximity domain is split across two fields for historical reasons.
struct SRAT_Processor_LAPIC {
    SRATSubtableHeader h;
    uint8_t  proximityDomain_lo;      // bits [7:0]
    uint8_t  apicId;
    uint32_t flags;                   // bit 0 = enabled
    uint8_t  localSapicEid;
    uint8_t  proximityDomain_hi[3];   // bits [31:8]
    uint32_t clockDomain;
} __attribute__((packed));

// Type 1: Memory affinity.
struct SRAT_Memory {
    SRATSubtableHeader h;
    uint32_t proximityDomain;
    uint16_t reserved1;
    uint32_t baseAddressLow;
    uint32_t baseAddressHigh;
    uint32_t lengthLow;
    uint32_t lengthHigh;
    uint32_t reserved2;
    uint32_t flags;                   // bit 0 = enabled, bit 2 = non-volatile
    uint64_t reserved3;
} __attribute__((packed));

// Type 2: Processor Local x2APIC affinity.
struct SRAT_Processor_x2APIC {
    SRATSubtableHeader h;
    uint16_t reserved;
    uint32_t proximityDomain;
    uint32_t x2apicId;
    uint32_t flags;                   // bit 0 = enabled
    uint32_t clockDomain;
    uint32_t reserved2;
} __attribute__((packed));

// Type 5: Generic initiator affinity.
struct SRAT_Generic_Initiator {
    SRATSubtableHeader h;
    uint8_t  reserved1;
    uint8_t  deviceHandleType;        // 0 = ACPI object handle, 1 = PCI SBDF
    uint32_t proximityDomain;
    uint8_t  deviceHandle[16];
    uint32_t flags;                   // bit 0 = enabled
    uint32_t reserved2;
} __attribute__((packed));

struct SRAT {
    SDTHeader          h;
    uint32_t           tableRevision;
    uint64_t           reserved;
    SRATSubtableHeader firstEntry;
} __attribute__((packed));

// ---- SLIT ----

struct SLIT {
    SDTHeader h;
    uint64_t  numLocalities;
    uint8_t   entries[1]; // numLocalities * numLocalities bytes
} __attribute__((packed));

// ---- HMAT ----

struct HMATStructureHeader {
    uint16_t type;
    uint16_t reserved;
    uint32_t length;
} __attribute__((packed));

// HMAT data type codes used in SLLBI structures.
enum class HMATDataType : uint8_t {
    AccessLatency   = 0,
    ReadLatency     = 1,
    WriteLatency    = 2,
    AccessBandwidth = 3,
    ReadBandwidth   = 4,
    WriteBandwidth  = 5
};

// Type 1: System Locality Latency and Bandwidth Information.
// Variable-length: followed by initiator domain list, target domain list, and matrix.
struct HMAT_SLLBI {
    HMATStructureHeader h;
    uint8_t  flags;
    uint8_t  dataType;             // HMATDataType
    uint8_t  minXferSize;
    uint8_t  reserved1;
    uint16_t numInitiatorDomains;
    uint16_t numTargetDomains;
    uint32_t reserved2;
    uint64_t entryBaseUnit;        // picoseconds (latency) or MB/s (bandwidth)
    // Followed by: uint32_t initiatorDomains[numInitiatorDomains]
    //              uint32_t targetDomains[numTargetDomains]
    //              uint16_t matrix[numInitiatorDomains * numTargetDomains]
} __attribute__((packed));

struct HMAT {
    SDTHeader           h;
    uint32_t            reserved;
    HMATStructureHeader firstStructure;
} __attribute__((packed));

// ---- CEDT (minimal; not fully parsed in this revision) ----

struct CEDT {
    SDTHeader h;
} __attribute__((packed));

} // namespace kernel::acpi

#pragma GCC diagnostic pop

// ============================================================================
// Iterator / range types
// ============================================================================

namespace kernel::acpi::numa {

using namespace kernel::numa;

// Internal helpers — returns the byte-range that covers SRAT subtable entries.
static inline const SRATSubtableHeader* sratBegin(const SRAT* srat) {
    if (!srat) return nullptr;
    return &srat->firstEntry;
}
static inline const SRATSubtableHeader* sratEnd(const SRAT* srat) {
    if (!srat) return nullptr;
    return reinterpret_cast<const SRATSubtableHeader*>(
        reinterpret_cast<const uint8_t*>(srat) + srat->h.length);
}

// ----------------------------------------------------------------------------
// SRATProcessorRange — yields ProcessorAffinityEntry
// Handles both LAPIC (type 0) and x2APIC (type 2) entries.
// Requires arch::amd64::smp::populateProcessorInfo() to have been called first.
// ----------------------------------------------------------------------------
class SRATProcessorRange {
    const SRAT* srat;
public:
    explicit SRATProcessorRange(const SRAT* s) : srat(s) {}

    class Iterator {
        const SRATSubtableHeader* ptr;
        const SRATSubtableHeader* end;

        void advance();

    public:
        Iterator(const SRATSubtableHeader* p, const SRATSubtableHeader* e)
            : ptr(p), end(e) { advance(); }

        bool operator!=(const Iterator& o) const { return ptr != o.ptr; }
        ProcessorAffinityEntry operator*() const;  // decodes from ptr on demand
        Iterator& operator++();
    };

    Iterator begin() const { return {sratBegin(srat), sratEnd(srat)}; }
    Iterator end()   const {
        const auto* e = sratEnd(srat);
        return {e, e};
    }
};

// ----------------------------------------------------------------------------
// SRATMemoryRange — yields MemoryRangeAffinityEntry
// ----------------------------------------------------------------------------
class SRATMemoryRange {
    const SRAT* srat;
public:
    explicit SRATMemoryRange(const SRAT* s) : srat(s) {}

    class Iterator {
        const SRATSubtableHeader* ptr;
        const SRATSubtableHeader* end;

        void advance();

    public:
        Iterator(const SRATSubtableHeader* p, const SRATSubtableHeader* e)
            : ptr(p), end(e) { advance(); }

        bool operator!=(const Iterator& o) const { return ptr != o.ptr; }
        MemoryRangeAffinityEntry operator*() const;  // decodes from ptr on demand
        Iterator& operator++();
    };

    Iterator begin() const { return {sratBegin(srat), sratEnd(srat)}; }
    Iterator end()   const {
        const auto* e = sratEnd(srat);
        return {e, e};
    }
};

// ----------------------------------------------------------------------------
// SRATGenericInitRange — yields GenericInitiatorEntry (type 5)
// ----------------------------------------------------------------------------
class SRATGenericInitRange {
    const SRAT* srat;
public:
    explicit SRATGenericInitRange(const SRAT* s) : srat(s) {}

    class Iterator {
        const SRATSubtableHeader* ptr;
        const SRATSubtableHeader* end;

        // advance() only positions ptr — no cached current since DeviceHandle
        // is not default-constructible.  Decoding happens in operator*().
        void advance();

    public:
        Iterator(const SRATSubtableHeader* p, const SRATSubtableHeader* e)
            : ptr(p), end(e) { advance(); }

        bool operator!=(const Iterator& o) const { return ptr != o.ptr; }
        GenericInitiatorEntry operator*() const;  // decodes from ptr on demand
        Iterator& operator++();
    };

    Iterator begin() const { return {sratBegin(srat), sratEnd(srat)}; }
    Iterator end()   const {
        const auto* e = sratEnd(srat);
        return {e, e};
    }
};

// ----------------------------------------------------------------------------
// SLITRange — yields LatencyEntry (relative distances)
// Produces one entry per non-trivial (initiator, target) pair in the SLIT matrix.
// Zero entries in the SLIT matrix (unreachable) are skipped.
// ----------------------------------------------------------------------------
class SLITRange {
    const SLIT* slit;
public:
    explicit SLITRange(const SLIT* s) : slit(s) {}

    class Iterator {
        const SLIT* slit;
        size_t      row;
        size_t      col;
        LatencyEntry current;

        void decode();
        void skipToValid();

    public:
        Iterator(const SLIT* s, size_t r, size_t c)
            : slit(s), row(r), col(c) {
            if (slit) { skipToValid(); if (!atEnd()) decode(); }
        }

        bool atEnd() const {
            return !slit || row >= slit->numLocalities;
        }

        bool operator!=(const Iterator& o) const {
            return row != o.row || col != o.col;
        }
        const LatencyEntry& operator*() const { return current; }
        Iterator& operator++();
    };

    Iterator begin() const;
    Iterator end()   const;
};

// ----------------------------------------------------------------------------
// HMATLatencyRange — yields LatencyEntry (absolute picoseconds)
// Iterates over all SLLBI structures with data_type in {0,1,2}.
// ----------------------------------------------------------------------------
class HMATLatencyRange {
    const HMAT* hmat;
public:
    explicit HMATLatencyRange(const HMAT* h) : hmat(h) {}

    class Iterator {
        const HMATStructureHeader* structPtr;
        const HMATStructureHeader* structEnd;
        size_t                     row;
        size_t                     col;
        LatencyEntry               current;

        bool         isLatencyStruct(const HMATStructureHeader* h) const;
        const HMAT_SLLBI* asSLLBI() const;
        const uint32_t* initiatorList() const;
        const uint32_t* targetList() const;
        const uint16_t* matrixData() const;
        void         advanceToNextStruct();
        void         skipToValid();
        void         decode();

    public:
        Iterator(const HMATStructureHeader* p, const HMATStructureHeader* e,
                 size_t r, size_t c)
            : structPtr(p), structEnd(e), row(r), col(c) {
            advanceToNextStruct();
            if (structPtr != structEnd) { skipToValid(); decode(); }
        }

        bool operator!=(const Iterator& o) const {
            return structPtr != o.structPtr || row != o.row || col != o.col;
        }
        const LatencyEntry& operator*() const { return current; }
        Iterator& operator++();
    };

    Iterator begin() const;
    Iterator end()   const;
};

// ----------------------------------------------------------------------------
// HMATBandwidthRange — yields BandwidthEntry (MB/s)
// Iterates over all SLLBI structures with data_type in {3,4,5}.
// ----------------------------------------------------------------------------
class HMATBandwidthRange {
    const HMAT* hmat;
public:
    explicit HMATBandwidthRange(const HMAT* h) : hmat(h) {}

    class Iterator {
        const HMATStructureHeader* structPtr;
        const HMATStructureHeader* structEnd;
        size_t                     row;
        size_t                     col;
        BandwidthEntry             current;

        bool          isBandwidthStruct(const HMATStructureHeader* h) const;
        const HMAT_SLLBI* asSLLBI() const;
        const uint32_t*   initiatorList() const;
        const uint32_t*   targetList() const;
        const uint16_t*   matrixData() const;
        void          advanceToNextStruct();
        void          skipToValid();
        void          decode();

    public:
        Iterator(const HMATStructureHeader* p, const HMATStructureHeader* e,
                 size_t r, size_t c)
            : structPtr(p), structEnd(e), row(r), col(c) {
            advanceToNextStruct();
            if (structPtr != structEnd) { skipToValid(); decode(); }
        }

        bool operator!=(const Iterator& o) const {
            return structPtr != o.structPtr || row != o.row || col != o.col;
        }
        const BandwidthEntry& operator*() const { return current; }
        Iterator& operator++();
    };

    Iterator begin() const;
    Iterator end()   const;
};

// ============================================================================
// Top-level factory function
// ============================================================================

// Reads all available NUMA ACPI tables (SRAT, HMAT/SLIT) and returns a fully
// constructed NUMATopology.  HMAT takes precedence over SLIT.
// If SRAT is absent the result is a trivial single-domain topology.
NUMATopology buildNUMATopology();

} // namespace kernel::acpi::numa

#endif // CROCOS_NUMA_ITERATORS_H
