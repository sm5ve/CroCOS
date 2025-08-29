//
// Created by Spencer Martin on 8/27/25.
//
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/amd64.h>
#include <core/ds/Trees.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>

namespace kernel::amd64::interrupts{
    constexpr uint32_t IOAPIC_REG_ID = 0x00;
    constexpr uint32_t IOAPIC_REG_VERSION = 0x01;
    constexpr uint32_t IOAPIC_REG_ARBITRATION_PRIORITY = 0x02;
    constexpr uint32_t IOAPIC_REG_REDIRECT_TABLE_BASE = 0x10;

    constexpr size_t IOAPIC_VECTOR_MAPPING_BASE = 0x10;

    IOAPIC::IOAPIC(uint8_t i, void* m, uint32_t g) : id(i), mmio_window(static_cast<volatile uint32_t*>(m)), gsi_base(g) {
        const uint32_t version = regRead(IOAPIC_REG_VERSION);
        lineCount = (version >> 16) & 0xffu;
    }

    IOAPIC::~IOAPIC() {}

    uint32_t IOAPIC::regRead(uint8_t index) const{
        mmio_window[0] = static_cast<uint32_t>(index) & 0xffu;
        return mmio_window[4];
    }

    void IOAPIC::regWrite(uint8_t index, uint32_t value){
        mmio_window[0] = static_cast<uint32_t>(index) & 0xffu;
        mmio_window[4] = value;
    }

    uint8_t getRegStartForLineIndex(size_t lineIndex){
        return static_cast<uint8_t>(lineIndex * 2 + 0x10);
    }

    void IOAPIC::setActivationType(uint32_t gsi, InterruptLineActivationType type) {
        assert(gsi - gsi_base < lineCount, "gsi out of range");
        auto regVal = regRead(getRegStartForLineIndex(gsi - gsi_base));
        constexpr uint32_t polarity_mask = 1u << 13;
        constexpr uint32_t trigger_mask = 1u << 15;
        regVal &= ~(polarity_mask | trigger_mask);
        if (isLevelTriggered(type)) regVal |= trigger_mask;
        if (isLowTriggered(type)) regVal |= polarity_mask;
        regWrite(getRegStartForLineIndex(gsi - gsi_base), regVal);
    }

    void IOAPIC::setNonmaskable(uint32_t gsi, bool nonmaskable) {
        assert(gsi - gsi_base < lineCount, "gsi out of range");
        auto regVal = regRead(getRegStartForLineIndex(gsi - gsi_base));
        constexpr uint32_t delivery_mask = 5u << 8;
        regVal &= ~delivery_mask;
        if (nonmaskable) {
            regVal |= 4u << 8;
        }
        regWrite(getRegStartForLineIndex(gsi - gsi_base), regVal);
    }

    size_t IOAPIC::getReceiverCount(){
        return lineCount;
    }

    size_t IOAPIC::getEmitterCount(){
        //From the OSDev wiki: allowed values for interrupt vectors are 0x10 to 0xFE
        return (INTERRUPT_VECTOR_COUNT - 2) - IOAPIC_VECTOR_MAPPING_BASE + 1ul;
    }

    bool IOAPIC::routeInterrupt(size_t lineIndex, size_t destinationLine){
        if (destinationLine < 0x10 || destinationLine > 0xFE) return false;
        if (lineIndex >= lineCount) return false;
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        regVal &= ~0xffu;
        regVal |= (static_cast<uint32_t>(destinationLine) & 0xff);
        regWrite(getRegStartForLineIndex(lineIndex), regVal);
        return true;
    }

    constexpr uint32_t IOAPIC_MASK_BIT = 1u << 16;

    bool IOAPIC::isReceiverMasked(size_t lineIndex) const{
        assert(lineIndex < lineCount, "lineIndex out of range");
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        return (regVal & IOAPIC_MASK_BIT) != 0;
    }

    void IOAPIC::setReceiverMask(size_t lineIndex, bool shouldMask){
        assert(lineIndex < lineCount, "lineIndex out of range");
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        regVal &= ~IOAPIC_MASK_BIT;
        if (shouldMask) regVal |= IOAPIC_MASK_BIT;
        regWrite(getRegStartForLineIndex(lineIndex), regVal);
    }

    uint32_t IOAPIC::getGSIBase() const {
        return gsi_base;
    }


    struct IOAPIC_GSI_Comparator {
        bool operator()(const SharedPtr<IOAPIC>& a, const SharedPtr<IOAPIC>& b){
            return a -> getGSIBase() < b -> getGSIBase();
        }
    };

    IRQDomain::IRQDomain(size_t mapping[16]){
        memcpy(surjectiveMapping, mapping, sizeof(surjectiveMapping));
        maxMapping = 0;
        for (auto i : surjectiveMapping) {
            maxMapping = max(i, maxMapping);
        }
    }

    size_t IRQDomain::getEmitterCount() {
        return maxMapping + 1;
    }

    size_t IRQDomain::getReceiverCount() {
        return 16;
    }

    size_t IRQDomain::getEmitterFor(size_t receiver) const{
        assert(receiver < 16, "receiver out of range");
        return surjectiveMapping[receiver];
    }

    IRQToIOAPICConnector::IRQToIOAPICConnector(SharedPtr<IRQDomain> irqDomain, SharedPtr<InterruptDomain> ioapic, Bimap<size_t, size_t> &&map) :
    DomainConnector(static_pointer_cast<InterruptDomain>(move(irqDomain)), move(ioapic)), irqToAPICLineMap(move(map)){}

    Optional<DomainInputIndex> IRQToIOAPICConnector::fromOutput(DomainOutputIndex index) const {
        if (irqToAPICLineMap.contains(index)) {
            return irqToAPICLineMap.at(index);
        }
        return {};
    }

    Optional<DomainOutputIndex> IRQToIOAPICConnector::fromInput(DomainInputIndex index) const {
        if (irqToAPICLineMap.containsRight(index)) {
            return irqToAPICLineMap.atRight(index);
        }
        return {};
    }

    using IOAPIC_Tree = RedBlackTree<SharedPtr<IOAPIC>, IOAPIC_GSI_Comparator>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_Tree, ioapicsByGSI);
    using IOAPIC_ID_Map = HashMap<size_t, SharedPtr<IOAPIC>>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_ID_Map, ioapicsByID);

    void createIOAPICStructures(acpi::MADT& madt) {
        for (const auto ioapicEntry : madt.entries<acpi::MADT_IOAPIC_Entry>()) {
            uintptr_t base = ioapicEntry.ioapicAddress;
            auto* mmio_window = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(base));
            auto gsiBase = ioapicEntry.gsiBase;
            auto ioapic = make_shared<IOAPIC>(ioapicEntry.ioapicID, mmio_window, gsiBase);
            ioapicsByID[ioapicEntry.ioapicID] = ioapic;
            ioapicsByGSI.insert(ioapic);
            hal::interrupts::topology::registerDomain(ioapic);

            auto apicConnector = make_shared<AffineConnector>(ioapic,
            getCPUInterruptVectors(), IOAPIC_VECTOR_MAPPING_BASE);
            hal::interrupts::topology::registerConnector(apicConnector);
        }
    }

    SharedPtr<IOAPIC> getIOAPICForGSI(uint32_t gsi) {
        SharedPtr<IOAPIC> apicForGSI;
        bool found = ioapicsByGSI.mappedFloor(gsi, apicForGSI, [](SharedPtr<IOAPIC> apic) {
            return apic -> getGSIBase();
        });
        if (!found) {return SharedPtr<IOAPIC>();}
        if (gsi - apicForGSI -> getGSIBase() >= apicForGSI -> getReceiverCount()) {
            return SharedPtr<IOAPIC>();
        }
        return apicForGSI;
    }

    InterruptLineActivationType getActivationTypeFromMADTFlags(uint16_t flags) {
        bool activeHigh = (flags & 2) == 0;
        bool edgeTriggered = (flags & 8) == 0;
        return activationTypeForLevelAndTriggerMode(activeHigh, edgeTriggered);
    }

    SharedPtr<IOAPIC> addIRQDomainConectorMapping(Optional<size_t>* irqToEmitterMap, size_t& emitterMax, HashMap<SharedPtr<IOAPIC>, SharedPtr<Bimap<size_t, size_t>>>& connectorMapsByIOAPIC, uint8_t irqSource, uint32_t gsi) {
        assert(irqSource < 16, "irqSource out of range");

        auto ioapic = getIOAPICForGSI(gsi);
        assert(ioapic, "No IOAPIC for GSI");
        if (!connectorMapsByIOAPIC.contains(ioapic)) {
            connectorMapsByIOAPIC.insert(ioapic, make_shared<Bimap<size_t, size_t>>());
        }

        const auto bimap = connectorMapsByIOAPIC[ioapic];
        //If the GSI we're trying to map to is already associated with an emitter, just update the emitter map
        //but no changes need to be made to the connector
        if (bimap -> containsRight(gsi)) {
            irqToEmitterMap[irqSource] = bimap -> atRight(gsi);
            return ioapic;
        }
        //Otherwise, we need to create a new emitter
        assert(!irqToEmitterMap[irqSource].occupied(), "why are we calling this on an already mapped irq source?");
        size_t emitterIndex = emitterMax++;
        irqToEmitterMap[irqSource] = emitterIndex;

        bimap -> insert(emitterIndex, gsi - ioapic -> getGSIBase());
        return ioapic;
    }

    SharedPtr<IRQDomain> irqDomain;

    void createIRQDomainConnectorsAndConfigureIOAPICActivationType(acpi::MADT& madt) {
        Optional<size_t> irqToEmitterMap[16];
        size_t emitterMax = 0;
        //maps IRQDomain emitter index to IOAPIC line (so gsi - gsi_base)
        HashMap<SharedPtr<IOAPIC>, SharedPtr<Bimap<size_t, size_t>>> connectorMapsByIOAPIC;
        uint16_t mappedIRQs = 0;
        //For every source override entry, we configure the IOAPIC activation type accordingly
        //and add the mapping to a bimap
        for (const auto& sourceOverride : madt.entries<acpi::MADT_IOAPIC_Source_Override_Entry>()) {
            if (sourceOverride.busSource != 0) {
                klog << "Warning: MADT interrupt source override entry lists non-ISA bus source.\n";
            }
            if (mappedIRQs & (1u << sourceOverride.irqSource)) {
                klog << "Warning: MADT interrupt source override entry lists duplicate interrupt source. Skipping.\n";
                continue;
            }
            auto ioapic = addIRQDomainConectorMapping(irqToEmitterMap, emitterMax, connectorMapsByIOAPIC, sourceOverride.irqSource, sourceOverride.gsi);
            ioapic -> setActivationType(sourceOverride.gsi, getActivationTypeFromMADTFlags(sourceOverride.flags));
            mappedIRQs |= 1u << sourceOverride.irqSource;
        }
        for (uint32_t i = 0; i < 16; i++) {
            if (mappedIRQs & (1u << i)) {
                continue;
            }
            addIRQDomainConectorMapping(irqToEmitterMap, emitterMax, connectorMapsByIOAPIC, static_cast<uint8_t>(i), i);
        }
        size_t finalizedEmitterMap[16];
        for (auto i = 0; i < 16; i++) {
            assert(irqToEmitterMap[i].occupied(), "irqToEmitterMap[i] not occupied");
            finalizedEmitterMap[i] = *irqToEmitterMap[i];
        }

        irqDomain = make_shared<IRQDomain>(finalizedEmitterMap);
        hal::interrupts::topology::registerDomain(irqDomain);
        for (const auto& connectorInfo : connectorMapsByIOAPIC) {
            auto ioapic = connectorInfo.first();
            auto bimap = connectorInfo.second();
            auto connector = make_shared<IRQToIOAPICConnector>(irqDomain, ioapic, move(*bimap));
            hal::interrupts::topology::registerConnector(connector);
        }
    }

    SharedPtr<IRQDomain> getIRQDomain() {
        return irqDomain;
    }

    void setupIOAPICs(acpi::MADT& madt) {
        createIOAPICStructures(madt);
        createIRQDomainConnectorsAndConfigureIOAPICActivationType(madt);
        //hal::interrupts::topology::registerDomain()
    }
}
