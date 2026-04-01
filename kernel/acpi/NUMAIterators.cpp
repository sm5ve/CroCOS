//
// Created by Spencer Martin on 3/28/26.
//
// ACPI SRAT / SLIT / HMAT iterator implementations.
//
// Reads hardware-description tables and produces typed entry values for
// NUMATopology::build().  LAPIC-to-logical-CPU translation is performed here
// using arch::amd64::smp facilities.
//

#include <acpi/NUMAIterators.h>
#include <kernel.h>
#include <assert.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

namespace kernel::acpi::numa {

using kernel::klog;

using namespace kernel::numa;

// ============================================================================
// Shared helper: advance a subtable pointer by one entry
// ============================================================================

static inline const SRATSubtableHeader* nextSubtable(const SRATSubtableHeader* h) {
    return reinterpret_cast<const SRATSubtableHeader*>(
        reinterpret_cast<const uint8_t*>(h) + h->length);
}

// ============================================================================
// SRATProcessorRange
// ============================================================================

static bool decodeLAPIC(const SRATSubtableHeader* h,
                         ProcessorAffinityEntry& out)
{
    const auto* e = reinterpret_cast<const SRAT_Processor_LAPIC*>(h);
    if (!(e->flags & 1u)) return false; // not enabled

    // Reconstruct full 32-bit proximity domain from split fields.
    const uint32_t rawDomain =
        static_cast<uint32_t>(e->proximityDomain_lo)
        | (static_cast<uint32_t>(e->proximityDomain_hi[0]) << 8)
        | (static_cast<uint32_t>(e->proximityDomain_hi[1]) << 16)
        | (static_cast<uint32_t>(e->proximityDomain_hi[2]) << 24);

    // Translate LAPIC ID to logical CPU ID.
    const auto& info = arch::amd64::smp::getProcessorInfoForLapicID(e->apicId);
    out.cpuId             = info.logicalID;
    out.rawDomainId       = rawDomain;
    out.rawClockDomainId  = e->clockDomain;
    return true;
}

static bool decodeX2APIC(const SRATSubtableHeader* h,
                          ProcessorAffinityEntry& out)
{
    const auto* e = reinterpret_cast<const SRAT_Processor_x2APIC*>(h);
    if (!(e->flags & 1u)) return false;

    // The x2APIC ID may exceed the uint8_t range used by LAPIC IDs.
    // For IDs that fit in uint8_t, reuse the LAPIC lookup.
    // For larger IDs, we would need a separate lookup — log and skip for now.
    if (e->x2apicId > 0xFFu) {
        klog() << "[NUMA] Warning: x2APIC ID " << e->x2apicId
                       << " > 0xFF; skipping (x2APIC > 255 not yet supported)\n";
        return false;
    }

    const auto& info =
        arch::amd64::smp::getProcessorInfoForLapicID(
            static_cast<uint8_t>(e->x2apicId));
    out.cpuId            = info.logicalID;
    out.rawDomainId      = e->proximityDomain;
    out.rawClockDomainId = e->clockDomain;
    return true;
}

void SRATProcessorRange::Iterator::advance() {
    while (ptr != end) {
        if (ptr->length == 0) {
            klog() << "[NUMA] Warning: SRAT subtable with zero length — aborting\n";
            ptr = end;
            return;
        }
        if (ptr->type == SRAT_PROCESSOR_LAPIC) {
            const auto* e = reinterpret_cast<const SRAT_Processor_LAPIC*>(ptr);
            if (e->flags & 1u) return; // enabled — stop here
        } else if (ptr->type == SRAT_PROCESSOR_X2APIC) {
            const auto* e = reinterpret_cast<const SRAT_Processor_x2APIC*>(ptr);
            if (e->flags & 1u) return; // enabled — stop here
        }
        ptr = nextSubtable(ptr);
    }
}

ProcessorAffinityEntry SRATProcessorRange::Iterator::operator*() const {
    ProcessorAffinityEntry out;
    if (ptr->type == SRAT_PROCESSOR_LAPIC)
        decodeLAPIC(ptr, out);
    else
        decodeX2APIC(ptr, out);
    return out;
}

SRATProcessorRange::Iterator& SRATProcessorRange::Iterator::operator++() {
    ptr = nextSubtable(ptr);
    advance();
    return *this;
}

// ============================================================================
// SRATMemoryRange
// ============================================================================

static bool decodeMemory(const SRATSubtableHeader* h,
                          MemoryRangeAffinityEntry& out)
{
    const auto* e = reinterpret_cast<const SRAT_Memory*>(h);
    if (!(e->flags & 1u)) return false; // not enabled

    const uint64_t base =
        static_cast<uint64_t>(e->baseAddressLow)
        | (static_cast<uint64_t>(e->baseAddressHigh) << 32);
    const uint64_t len =
        static_cast<uint64_t>(e->lengthLow)
        | (static_cast<uint64_t>(e->lengthHigh) << 32);

    if (len == 0) return false;

    out.range       = { mm::phys_addr(base), mm::phys_addr(base + len) };
    out.rawDomainId = e->proximityDomain;
    out.memType     = (e->flags & (1u << 2)) ? MemoryType::Nonvolatile
                                             : MemoryType::Volatile;
    return true;
}

void SRATMemoryRange::Iterator::advance() {
    while (ptr != end) {
        if (ptr->length == 0) {
            klog() << "[NUMA] Warning: SRAT subtable with zero length — aborting\n";
            ptr = end;
            return;
        }
        if (ptr->type == SRAT_MEMORY) {
            const auto* e = reinterpret_cast<const SRAT_Memory*>(ptr);
            if ((e->flags & 1u) && (e->lengthLow | e->lengthHigh)) return; // enabled, non-zero
        }
        ptr = nextSubtable(ptr);
    }
}

MemoryRangeAffinityEntry SRATMemoryRange::Iterator::operator*() const {
    MemoryRangeAffinityEntry out;
    decodeMemory(ptr, out);
    return out;
}

SRATMemoryRange::Iterator& SRATMemoryRange::Iterator::operator++() {
    ptr = nextSubtable(ptr);
    advance();
    return *this;
}

// ============================================================================
// SRATGenericInitRange
// ============================================================================

void SRATGenericInitRange::Iterator::advance() {
    while (ptr != end) {
        if (ptr->length == 0) {
            klog() << "[NUMA] Warning: SRAT subtable with zero length — aborting\n";
            ptr = end;
            return;
        }
        if (ptr->type == SRAT_GENERIC_INITIATOR) {
            const auto* e = reinterpret_cast<const SRAT_Generic_Initiator*>(ptr);
            if (e->flags & 1u) return;  // enabled entry found — stop here
        }
        ptr = nextSubtable(ptr);
    }
}

GenericInitiatorEntry SRATGenericInitRange::Iterator::operator*() const {
    // ptr is guaranteed to point at a valid, enabled type-5 entry.
    const auto* e = reinterpret_cast<const SRAT_Generic_Initiator*>(ptr);

    if (e->deviceHandleType == 1) {
        // PCI SBDF: bytes 0-1 = segment, byte 2 = bus, byte 3 = device|function
        PciSBDF sbdf;
        sbdf.segment  = static_cast<uint16_t>(
                            static_cast<uint16_t>(e->deviceHandle[0])
                            | static_cast<uint16_t>(e->deviceHandle[1] << 8u));
        sbdf.bus      = e->deviceHandle[2];
        sbdf.device   = (e->deviceHandle[3] >> 3) & 0x1Fu;
        sbdf.function = e->deviceHandle[3] & 0x07u;
        return { DeviceHandle(sbdf), e->proximityDomain };
    }

    // ACPI object handle: treat first 4 bytes as a 32-bit handle
    const uint32_t handle =
        static_cast<uint32_t>(e->deviceHandle[0])
        | (static_cast<uint32_t>(e->deviceHandle[1]) << 8)
        | (static_cast<uint32_t>(e->deviceHandle[2]) << 16)
        | (static_cast<uint32_t>(e->deviceHandle[3]) << 24);
    return { DeviceHandle(AcpiHandle{handle}), e->proximityDomain };
}

SRATGenericInitRange::Iterator& SRATGenericInitRange::Iterator::operator++() {
    ptr = nextSubtable(ptr);
    advance();
    return *this;
}

// ============================================================================
// SLITRange
// ============================================================================

void SLITRange::Iterator::skipToValid() {
    const size_t n = static_cast<size_t>(slit->numLocalities);
    while (row < n) {
        assert(row < n && col < n, "SLIT: row/col index out of bounds");
        // SLIT value 0 = unreachable, 0xFF = also unreachable (firmware convention)
        const uint8_t val = slit->entries[row * n + col];
        if (val != 0 && val != 0xFFu) return;
        // Advance col, then row
        if (++col >= n) { ++row; col = 0; }
    }
}

void SLITRange::Iterator::decode() {
    const size_t n = static_cast<size_t>(slit->numLocalities);
    const uint8_t val = slit->entries[row * n + col];
    current.rawInitiatorDomainId = static_cast<uint32_t>(row);
    current.rawTargetDomainId    = static_cast<uint32_t>(col);
    current.readLatency          = static_cast<uint64_t>(val);
    current.writeLatency         = static_cast<uint64_t>(val);
    current.absolute             = false; // SLIT = relative distances
}

SLITRange::Iterator& SLITRange::Iterator::operator++() {
    const size_t n = static_cast<size_t>(slit->numLocalities);
    if (++col >= n) { ++row; col = 0; }
    skipToValid();
    if (!atEnd()) decode();
    return *this;
}

SLITRange::Iterator SLITRange::begin() const {
    if (!slit) return {nullptr, 0, 0};
    return {slit, 0, 0};
}

SLITRange::Iterator SLITRange::end() const {
    if (!slit) return {nullptr, 0, 0};
    const size_t n = static_cast<size_t>(slit->numLocalities);
    return {slit, n, 0};
}

// ============================================================================
// HMAT helpers — shared between HMATLatencyRange and HMATBandwidthRange
// ============================================================================

static inline const HMATStructureHeader* hmatBegin(const HMAT* hmat) {
    if (!hmat) return nullptr;
    return &hmat->firstStructure;
}

static inline const HMATStructureHeader* hmatEnd(const HMAT* hmat) {
    if (!hmat) return nullptr;
    return reinterpret_cast<const HMATStructureHeader*>(
        reinterpret_cast<const uint8_t*>(hmat) + hmat->h.length);
}

static inline const HMATStructureHeader* nextHMATStruct(const HMATStructureHeader* h) {
    return reinterpret_cast<const HMATStructureHeader*>(
        reinterpret_cast<const uint8_t*>(h) + h->length);
}

// Accessors for the variable-length data following an HMAT_SLLBI header.
static const uint32_t* sllbiInitiatorList(const HMAT_SLLBI* s) {
    return reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(s) + sizeof(HMAT_SLLBI));
}
static const uint32_t* sllbiTargetList(const HMAT_SLLBI* s) {
    return sllbiInitiatorList(s) + s->numInitiatorDomains;
}
static const uint16_t* sllbiMatrix(const HMAT_SLLBI* s) {
    return reinterpret_cast<const uint16_t*>(
        sllbiTargetList(s) + s->numTargetDomains);
}

// ============================================================================
// HMATLatencyRange
// ============================================================================

bool HMATLatencyRange::Iterator::isLatencyStruct(const HMATStructureHeader* h) const {
    if (h->type != 1) return false;
    const auto* s = reinterpret_cast<const HMAT_SLLBI*>(h);
    const auto dt = static_cast<HMATDataType>(s->dataType);
    return dt == HMATDataType::AccessLatency
        || dt == HMATDataType::ReadLatency
        || dt == HMATDataType::WriteLatency;
}

const HMAT_SLLBI* HMATLatencyRange::Iterator::asSLLBI() const {
    return reinterpret_cast<const HMAT_SLLBI*>(structPtr);
}

const uint32_t* HMATLatencyRange::Iterator::initiatorList() const {
    return sllbiInitiatorList(asSLLBI());
}
const uint32_t* HMATLatencyRange::Iterator::targetList() const {
    return sllbiTargetList(asSLLBI());
}
const uint16_t* HMATLatencyRange::Iterator::matrixData() const {
    return sllbiMatrix(asSLLBI());
}

void HMATLatencyRange::Iterator::advanceToNextStruct() {
    while (structPtr != structEnd && !isLatencyStruct(structPtr)) {
        if (structPtr->length == 0) {
            klog() << "[NUMA] Warning: HMAT structure with zero length — aborting\n";
            structPtr = structEnd;
            return;
        }
        structPtr = nextHMATStruct(structPtr);
    }
}

void HMATLatencyRange::Iterator::skipToValid() {
    while (structPtr != structEnd) {
        const auto* s = asSLLBI();
        while (row < s->numInitiatorDomains) {
            const uint16_t v = matrixData()[row * s->numTargetDomains + col];
            if (v != 0 && v != 0xFFFFu) return;
            if (++col >= s->numTargetDomains) { ++row; col = 0; }
        }
        // Exhausted this structure — advance to the next latency structure
        structPtr = nextHMATStruct(structPtr);
        advanceToNextStruct();
        row = 0; col = 0;
    }
}

void HMATLatencyRange::Iterator::decode() {
    const auto* s   = asSLLBI();
    const uint16_t v = matrixData()[row * s->numTargetDomains + col];
    const uint64_t actualValue = static_cast<uint64_t>(v) * s->entryBaseUnit;

    current.rawInitiatorDomainId = initiatorList()[row];
    current.rawTargetDomainId    = targetList()[col];
    current.absolute             = true;

    const auto dt = static_cast<HMATDataType>(s->dataType);
    if (dt == HMATDataType::WriteLatency) {
        current.readLatency  = DISTANCE_NO_DATA;
        current.writeLatency = actualValue;
    } else if (dt == HMATDataType::ReadLatency) {
        current.readLatency  = actualValue;
        current.writeLatency = DISTANCE_NO_DATA;
    } else {
        // AccessLatency — representative of both directions
        current.readLatency  = actualValue;
        current.writeLatency = actualValue;
    }
}

HMATLatencyRange::Iterator& HMATLatencyRange::Iterator::operator++() {
    const auto* s = asSLLBI();
    if (++col >= s->numTargetDomains) { ++row; col = 0; }
    if (row < s->numInitiatorDomains) {
        skipToValid();
        if (structPtr != structEnd) decode();
    } else {
        structPtr = nextHMATStruct(structPtr);
        advanceToNextStruct();
        row = 0; col = 0;
        if (structPtr != structEnd) { skipToValid(); decode(); }
    }
    return *this;
}

HMATLatencyRange::Iterator HMATLatencyRange::begin() const {
    return {hmatBegin(hmat), hmatEnd(hmat), 0, 0};
}
HMATLatencyRange::Iterator HMATLatencyRange::end() const {
    const auto* e = hmatEnd(hmat);
    return {e, e, 0, 0};
}

// ============================================================================
// HMATBandwidthRange
// ============================================================================

bool HMATBandwidthRange::Iterator::isBandwidthStruct(const HMATStructureHeader* h) const {
    if (h->type != 1) return false;
    const auto* s = reinterpret_cast<const HMAT_SLLBI*>(h);
    const auto dt = static_cast<HMATDataType>(s->dataType);
    return dt == HMATDataType::AccessBandwidth
        || dt == HMATDataType::ReadBandwidth
        || dt == HMATDataType::WriteBandwidth;
}

const HMAT_SLLBI* HMATBandwidthRange::Iterator::asSLLBI() const {
    return reinterpret_cast<const HMAT_SLLBI*>(structPtr);
}
const uint32_t* HMATBandwidthRange::Iterator::initiatorList() const {
    return sllbiInitiatorList(asSLLBI());
}
const uint32_t* HMATBandwidthRange::Iterator::targetList() const {
    return sllbiTargetList(asSLLBI());
}
const uint16_t* HMATBandwidthRange::Iterator::matrixData() const {
    return sllbiMatrix(asSLLBI());
}

void HMATBandwidthRange::Iterator::advanceToNextStruct() {
    while (structPtr != structEnd && !isBandwidthStruct(structPtr)) {
        if (structPtr->length == 0) {
            klog() << "[NUMA] Warning: HMAT structure with zero length — aborting\n";
            structPtr = structEnd;
            return;
        }
        structPtr = nextHMATStruct(structPtr);
    }
}

void HMATBandwidthRange::Iterator::skipToValid() {
    while (structPtr != structEnd) {
        const auto* s = asSLLBI();
        while (row < s->numInitiatorDomains) {
            const uint16_t v = matrixData()[row * s->numTargetDomains + col];
            if (v != 0 && v != 0xFFFFu) return;
            if (++col >= s->numTargetDomains) { ++row; col = 0; }
        }
        structPtr = nextHMATStruct(structPtr);
        advanceToNextStruct();
        row = 0; col = 0;
    }
}

void HMATBandwidthRange::Iterator::decode() {
    const auto*   s = asSLLBI();
    const uint16_t v = matrixData()[row * s->numTargetDomains + col];
    const uint64_t actualValue = static_cast<uint64_t>(v) * s->entryBaseUnit;

    current.rawInitiatorDomainId = initiatorList()[row];
    current.rawTargetDomainId    = targetList()[col];

    const auto dt = static_cast<HMATDataType>(s->dataType);
    if (dt == HMATDataType::WriteBandwidth) {
        current.readBandwidth  = DISTANCE_NO_DATA;
        current.writeBandwidth = actualValue;
    } else if (dt == HMATDataType::ReadBandwidth) {
        current.readBandwidth  = actualValue;
        current.writeBandwidth = DISTANCE_NO_DATA;
    } else {
        // AccessBandwidth — representative of both directions
        current.readBandwidth  = actualValue;
        current.writeBandwidth = actualValue;
    }
}

HMATBandwidthRange::Iterator& HMATBandwidthRange::Iterator::operator++() {
    const auto* s = asSLLBI();
    if (++col >= s->numTargetDomains) { ++row; col = 0; }
    if (row < s->numInitiatorDomains) {
        skipToValid();
        if (structPtr != structEnd) decode();
    } else {
        structPtr = nextHMATStruct(structPtr);
        advanceToNextStruct();
        row = 0; col = 0;
        if (structPtr != structEnd) { skipToValid(); decode(); }
    }
    return *this;
}

HMATBandwidthRange::Iterator HMATBandwidthRange::begin() const {
    return {hmatBegin(hmat), hmatEnd(hmat), 0, 0};
}
HMATBandwidthRange::Iterator HMATBandwidthRange::end() const {
    const auto* e = hmatEnd(hmat);
    return {e, e, 0, 0};
}

// ============================================================================
// buildNUMATopology — top-level factory
// ============================================================================

NUMATopology buildNUMATopology() {
    const auto* srat = kernel::acpi::optional<kernel::acpi::SRAT>();
    const auto* slit = kernel::acpi::optional<kernel::acpi::SLIT>();
    const auto* hmat = kernel::acpi::optional<kernel::acpi::HMAT>();

    if (srat) {
        klog() << "[NUMA] SRAT present\n";
    } else {
        klog() << "[NUMA] No SRAT — trivial topology\n";
    }

    SRATProcessorRange  procRange(srat);
    SRATMemoryRange     memRange(srat);
    SRATGenericInitRange genRange(srat);

    // HMAT takes precedence over SLIT when both are present.
    if (hmat) {
        klog() << "[NUMA] Using HMAT for distance data\n";
        return NUMATopology::build(
            procRange, memRange, genRange,
            HMATLatencyRange(hmat),
            HMATBandwidthRange(hmat));
    }

    if (slit) {
        klog() << "[NUMA] Using SLIT for distance data\n";
        return NUMATopology::build(
            procRange, memRange, genRange,
            SLITRange(slit));
    }

    return NUMATopology::build(procRange, memRange, genRange);
}

} // namespace kernel::acpi::numa

#pragma GCC diagnostic pop
