//
// Created by Spencer Martin on 4/11/25.
//
#include <arch/amd64/interrupts/APIC.h>
#include <core/ds/HashMap.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/amd64.h>

namespace kernel::amd64::smp{
    Vector<ProcessorInfo>* processors;

    const ProcessorInfo& getProcessorInfoForLapicID(uint8_t lapicID){
        for(auto& entry : *processors){
            if(entry.lapicID == lapicID){
                return entry;
            }
        }
        assertNotReached("Unknown LAPIC ID");
    }

    const ProcessorInfo& getProcessorInfoForAcpiID(uint8_t acpiID){
        for(auto& entry : *processors){
            if(entry.acpiProcessorID == acpiID){
                return entry;
            }
        }
        assertNotReached("Unknown ACPI Processor ID");
    }
}

namespace kernel::amd64::interrupts{
    using namespace kernel::hal::interrupts::hardware;

    HashMap<uint16_t, uint32_t>* irqOverrideMap;

    /*class Lapic{
        const uint32_t LAPIC_REG_ID = 0x20;
        const uint32_t LAPIC_REG_VERSION = 0x30;

        void* lapicMapping;

        uint32_t& getLapicRegister(uint32_t offset){
            assert(offset % 16 == 0, "Misaligned LAPIC register read");
            return *(uint32_t*)((uint64_t)lapicMapping + offset);
        }

        void initializeLapic(){
            auto& info = smp::getProcessorInfoForLapicID(getLapicRegister(LAPIC_REG_ID) & 0xff);
            smp::setLogicalProcessorID(info.logicalID);
        }
    }*/

    Vector<IOapic*>* ioapics;
    IOapic* getIOapicForGSI(uint32_t gsi);

    ActivationType decodeInterruptOverrideFlags(uint16_t flags) {
        using enum ActivationType;

        bool activeLow = flags & (1 << 1);
        bool levelTriggered = flags & (1 << 3);

        if (levelTriggered) {
            return activeLow ? LevelLow : LevelHigh;
        } else {
            return activeLow ? EdgeLow : EdgeHigh;
        }
    }

    void buildApicTopology(acpi::MADT& madt){
        using namespace kernel::amd64::smp;
        processors = new Vector<ProcessorInfo>();
        for(auto& lapicEntry : madt.entries<acpi::MADT_LAPIC_Entry>()){
            processors->push({
                                     (hal::ProcessorID) processors->getSize(),
                                     lapicEntry.apicID,
                                     lapicEntry.acpiProcessorID
            });
        }

        auto lapicAddr = (uint64_t)madt.lapicAddr;
        size_t overrideTableCount = 0;
        for(auto& override : madt.entries<acpi::MADT_LAPIC_Address_Override_Entry>()){
            lapicAddr = override.lapicAddr;
            overrideTableCount++;
        }
        assert(overrideTableCount < 2, "Malformed MADT - has multiple LAPIC address override entries");

        kernel::klog << "lapic address is " << mm::phys_addr(lapicAddr) << "\n";
        assert(((uint64_t)madt.lapicAddr % 4096) == 0, "This temporary initialization assumes the LAPIC MMIO range is page aligned");
        //Lapic::lapicMapping = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(lapicAddr));
        //Lapic::initializeLapic();

        ioapics = new Vector<IOapic*>();
        for(auto& ioapicEntry : madt.entries<acpi::MADT_IOAPIC_Entry>()){
            assert(((uint64_t)ioapicEntry.ioapicAddress % 4096) == 0, "This temporary initialization assumes the IOAPIC MMIO range is page aligned");
            kernel::klog << "IOAPIC at " << mm::phys_addr(ioapicEntry.ioapicAddress) << "\n";
            void* ioapic_mmio = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(ioapicEntry.ioapicAddress));;
            ioapics -> push(new IOapic(ioapicEntry.ioapicID, ioapic_mmio, ioapicEntry.gsiBase));
        }

        irqOverrideMap = new HashMap<uint16_t, uint32_t>();
        for(auto& entry : madt.entries<acpi::MADT_IOAPIC_Source_Override_Entry>()){
            if(auto* ioapic = getIOapicForGSI(entry.gsi)){
                ioapic -> setActivationMode(entry.gsi, decodeInterruptOverrideFlags(entry.flags));
                irqOverrideMap -> insert(static_cast<unsigned short>(entry.irqSource | ((uint16_t) entry.busSource << 8)),
                                         entry.gsi);
            }
            else{
                kernel::klog << "MADT source override is for GSI not handled by any IOAPIC\n";
                kernel::klog << "ioapic redirection entry:\n";
                kernel::klog << "bus: " << entry.busSource << "\n";
                kernel::klog << "irq: " << entry.irqSource << "\n";
                kernel::klog << "gsi: " << entry.gsi << "\n";
                kernel::klog << "flags: " << (void*)(uint64_t)entry.flags << "\n";
            }
        }

        for(auto& entry : madt.entries<acpi::MADT_IOAPIC_NMI_Source_Entry>()){
            if(auto* ioapic = getIOapicForGSI(entry.gsi)){
                ioapic -> setActivationMode(entry.gsi, decodeInterruptOverrideFlags(entry.flags));
                ioapic -> setNonmaskable(entry.gsi);
            }
            else{
                kernel::klog << "MADT NMI source is for GSI not handled by any IOAPIC\n";
                kernel::klog << entry.gsi << "\n";
                kernel::klog << (void*)(uint64_t)entry.flags << "\n";
            }
        }
    }

    IOapic::IOapic(uint8_t id, void *mmio_window, uint32_t gsi) {
        apicID = id;
        mmio_base = (uint32_t*)mmio_window;
        gsi_base = gsi;
        assert(apicID == regRead(0x00), "IOapic ID mismatch");
        //The version register contains the highest index addressable in the redirection table, not its size. To get
        //the size, we add 1
        redirectionTableSize = ((regRead(0x01) >> 16) & 0xff) + 1;
        mappings = (Tuple<hardware::VectorIndex, InterruptCPUAffinity>*)
                (void*) new uint8_t[sizeof(Tuple<hardware::VectorIndex, InterruptCPUAffinity>) * redirectionTableSize];
        klog << "IOAPIC ID " << apicID << " has redirection table size " << redirectionTableSize << "\n";
        for(size_t i = 0; i < redirectionTableSize; i++){
            mappings[i] = Tuple<hardware::VectorIndex, InterruptCPUAffinity>(0, NontargetedAffinityTypes::Global);
        }
    }

    IOapic* getIOapicForGSI(uint32_t gsi){
        for(auto* apic : *ioapics){
            uint32_t offset = gsi - apic->getGSIBase();
            if(offset < apic -> getRedirectionTableSize()){
                return apic;
            }
        }
        return nullptr;
    }

    InterruptReceiver getInterruptReceiverForIRQ(uint8_t irqno, BusType bt){
        auto gsi = (uint32_t)irqno;
        auto key = static_cast<uint16_t>(irqno | ((uint16_t) bt << 8));
        if(irqOverrideMap -> contains(key)){
            gsi = (*irqOverrideMap)[key];
        }
        assert(getIOapicForGSI(gsi) != nullptr, "IRQ not mapped to any IOAPIC!!!");
        return getIOapicForGSI(gsi)->getReceiverForGSI(gsi);
    }

    InterruptControllerFeature IOapic::getFeatureSet() {
        return InterruptControllerFeature::AffinityLocallyConfigurable;
    }

    Optional<Tuple<VectorIndex, Optional<InterruptCPUAffinity>>>
    IOapic::getMapping(kernel::hal::interrupts::InterruptReceiver r) {
        auto index = indexForInterruptReceiver(r);
        if(!index.occupied()){
            kernel::klog << "Passed incorrect or malformed InterruptReceiver to IOAPIC\n";
            return {};
        }
        auto& m = mappings[*index];
        return Tuple<VectorIndex, Optional<InterruptCPUAffinity>>(m.get<0>(), m.get<1>());
    }

    bool
    IOapic::setMapping(kernel::hal::interrupts::InterruptReceiver r, kernel::hal::interrupts::hardware::VectorIndex vi,
                       Optional<kernel::hal::interrupts::InterruptCPUAffinity> aff) {
        auto index = indexForInterruptReceiver(r);
        if(!index.occupied()){
            kernel::klog << "Passed incorrect or malformed InterruptReceiver to IOAPIC\n";
            return false;
        }
        if(vi >= 0xff || vi < 0x10){
            //According to the IOAPIC documentation these interrupt vector indices are no good
            kernel::klog << "Tried to map redirection table entry to out of bounds vector " << vi << "\n";
            return false;
        }
        uint8_t physID = 0;
        if(aff){
            if(auto* pid = aff->get_if<hal::ProcessorID>()){
                physID = (*smp::processors)[*pid].lapicID;
                if(physID > 0xf){
                    //We can only address LAPIC IDs up to 0xf in physical destination mode, since we've only got 4 bits
                    kernel::klog << "Tried to set interrupt affinity to physical LAPIC address that is too large\n";
                    return false;
                }
            }
            else{
                //We don't yet support global mapping modes
                kernel::klog << "We don't set support global mapping modes for the LAPIC\n";
                return false;
            }
        }
        else{
            //If we don't specify an affinity, just map to processor 0
            physID = (*smp::processors)[0].lapicID;
            aff.emplace(hal::ProcessorID (0));
        }
        //Set the vector
        uint32_t reg0 = regRead(static_cast<uint8_t>(0x10 + *index * 2));
        reg0 &= ~0xffu;
        reg0 |= ((uint32_t)vi) & 0xff;
        //Make sure the delivery mode is physical
        reg0 &= ~(1u << 11); //Bit 11 = 0 means physical delivery, = 1 means logical
        regWrite(static_cast<uint8_t>(0x10 + *index * 2), reg0);
        //Set the delivery destination
        uint32_t reg1 = regRead(static_cast<uint8_t>(0x10 + *index * 2 + 1));
        reg1 &= ~(0x0fu << 24);
        reg1 |= static_cast<uint32_t>(physID << 24);
        regWrite(static_cast<uint8_t>(0x10 + *index * 2 + 1), reg1);
        //Finally update the redirection map cache
        mappings[*index] = Tuple<VectorIndex, InterruptCPUAffinity>(vi, *aff);
        return true;
    }

    bool IOapic::setInterruptMaskState(kernel::hal::interrupts::InterruptReceiver receiver, bool state) {
        auto index = indexForInterruptReceiver(receiver);
        if(!index.occupied()){
            kernel::klog << "Passed incorrect or malformed InterruptReceiver to IOAPIC\n";
            return false;
        }
        uint32_t reg = regRead(static_cast<uint8_t>(0x10 + *index * 2));
        reg &= ~(1u << 16); //Bit 16 is the interrupt mask
        if(state) reg |= (1u << 16);
        regWrite(static_cast<uint8_t>(0x10 + *index * 2), reg);
        return true;
    }

    bool IOapic::getInterruptMaskState(kernel::hal::interrupts::InterruptReceiver receiver) {
        auto index = indexForInterruptReceiver(receiver);
        if(!index.occupied()){
            kernel::klog << "Passed incorrect or malformed InterruptReceiver to IOAPIC\n";
            return false;
        }
        uint32_t reg = regRead(static_cast<uint8_t>(0x10 + *index * 2));
        return (reg >> 16) != 0; //if bit 16 is unset, the interrupt is unmasked
    }

    uint32_t IOapic::regRead(uint8_t index){
        mmio_base[0] = index & 0xff;
        return mmio_base[4];
    }

    void IOapic::regWrite(uint8_t index, uint32_t value){
        mmio_base[0] = index & 0xff;
        mmio_base[4] = value;
    }

    InterruptReceiver IOapic::getReceiverForGSI(uint32_t gsi){
        return InterruptReceiver{
            .owner = this,
            .metadata = gsi - gsi_base
        };
    }

    void IOapic::setActivationMode(uint32_t gsi, ActivationType at){
        auto gsiOffset = gsi - gsi_base;
        assert(gsiOffset < redirectionTableSize, "GSI out of bounds for IOAPIC");
        auto index = static_cast<uint8_t>(0x10 + gsiOffset * 2);
        auto oldReg = regRead(index);
        auto newReg = oldReg & ~((1u << 13) | (1u << 15));
        using enum hal::interrupts::ActivationType;
        switch (at) {
            case EdgeHigh:
                break;
            case EdgeLow:
                newReg |= (1u << 13);
                break;
            case LevelHigh:
                newReg |= (1u << 15);
                break;
            case LevelLow:
                newReg |= (1u << 13);
                newReg |= (1u << 15);
                break;
        }
        regWrite(index, newReg);
    }

    void IOapic::setNonmaskable(uint32_t gsi){
        auto gsiOffset = gsi - gsi_base;
        assert(gsiOffset < redirectionTableSize, "GSI out of bounds for IOAPIC");
        auto index = static_cast<uint8_t>(0x10 + gsiOffset * 2);
        uint32_t reg = regRead(index);
        reg &= ~(7u << 8);
        reg |= (4u << 8);
        regWrite(index, reg);
    }

    uint32_t IOapic::getGSIBase(){
        return gsi_base;
    }

    size_t IOapic::getRedirectionTableSize(){
        return redirectionTableSize;
    }

    Optional<uint32_t> IOapic::indexForInterruptReceiver(InterruptReceiver r){
        if(r.owner != (IInterruptController*)this){
            return {};
        }
        uint32_t index = (uint32_t)r.metadata - gsi_base;
        if(index < redirectionTableSize){
            return index;
        }
        return {};
    }
}