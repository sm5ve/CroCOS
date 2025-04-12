//
// Created by Spencer Martin on 4/11/25.
//
#include <arch/amd64/interrupts/APIC.h>
#include <lib/ds/HashMap.h>
#include <arch/amd64/smp.h>

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
        kernel::DbgOut << "lapic address is " << (void*)(uint64_t)madt.lapicAddr << "\n";
        kernel::DbgOut << "this function's address is " << (void*)buildApicTopology << "\n";
    }

    IOapic* getIOapicForGSI(uint32_t gsi){
        (void)gsi;
        return nullptr;
    }

    void initializeLapic(mm::phys_addr lapic_addr){
        (void)lapic_addr;
    }
}