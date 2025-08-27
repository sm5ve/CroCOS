//
// Created by Spencer Martin on 8/27/25.
//
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/amd64.h>
#include <core/ds/Trees.h>

namespace kernel::amd64::interrupts{
    constexpr uint32_t IOAPIC_REG_ID = 0x00;
    constexpr uint32_t IOAPIC_REG_VERSION = 0x01;
    constexpr uint32_t IOAPIC_REG_ARBITRATION_PRIORITY = 0x02;
    constexpr uint32_t IOAPIC_REG_REDIRECT_TABLE_BASE = 0x10;

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
        return 0xFEul - 0x10ul + 1ul;
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
    using IOAPIC_Tree = RedBlackTree<SharedPtr<IOAPIC>, IOAPIC_GSI_Comparator>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_Tree, ioapicsByGSI);
    using IOAPIC_ID_Map = HashMap<size_t, SharedPtr<IOAPIC>>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_ID_Map, ioapicsByID);

    void setupIOAPICs(acpi::MADT& madt) {
        for (auto ioapicEntry : madt.entries<acpi::MADT_IOAPIC_Entry>()) {
            uintptr_t base = ioapicEntry.ioapicAddress;
            auto* mmio_window = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(base));
            auto gsiBase = ioapicEntry.gsiBase;
            auto ioapic = make_shared<IOAPIC>(ioapicEntry.ioapicID, mmio_window, gsiBase);
            ioapicsByID[ioapicEntry.ioapicID] = ioapic;
            ioapicsByGSI.insert(ioapic);
            hal::interrupts::topology::registerDomain(ioapic);
        }
    }
}
