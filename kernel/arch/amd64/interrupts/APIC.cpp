//
// Created by Spencer Martin on 4/11/25.
//
#include <arch/amd64/interrupts/APIC.h>
#include <lib/ds/HashMap.h>
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
    using namespace kernel::hal::interrupts::topology;
    namespace Lapic{
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
    }

    Vector<IOapic*>* ioapics;

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

        kernel::DbgOut << "lapic address is " << mm::phys_addr(lapicAddr) << "\n";
        assert(((uint64_t)madt.lapicAddr % 4096) == 0, "This temporary initialization assumes the LAPIC MMIO range is page aligned");
        Lapic::lapicMapping = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(lapicAddr));
        Lapic::initializeLapic();

        ioapics = new Vector<IOapic*>();
        for(auto& ioapicEntry : madt.entries<acpi::MADT_IOAPIC_Entry>()){
            assert(((uint64_t)ioapicEntry.ioapicAddress % 4096) == 0, "This temporary initialization assumes the IOAPIC MMIO range is page aligned");
            kernel::DbgOut << "IOAPIC at " << mm::phys_addr(ioapicEntry.ioapicAddress) << "\n";
            void* ioapic_mmio = amd64::PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(ioapicEntry.ioapicAddress));;
            ioapics -> push(new IOapic(ioapicEntry.ioapicID, ioapic_mmio, ioapicEntry.gsiBase));
        }
    }

    IOapic::IOapic(uint8_t id, void *mmio_window, uint32_t gsi) {
        this -> apicID = id;
        this -> mmio_base = mmio_window;
        this -> gsi_base = gsi;
    }

    IOapic* getIOapicForGSI(uint32_t gsi){
        (void)gsi;
        return nullptr;
    }
}