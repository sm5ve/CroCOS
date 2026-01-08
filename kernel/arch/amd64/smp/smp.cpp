//
// Created by Spencer Martin on 4/12/25.
//

#include <acpi.h>
#include <timing.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/interrupts/APIC.h>
#include <kconfig.h>

namespace kernel::amd64::smp{
    ProcessorInfo* pinfo;

    void setLogicalProcessorID(hal::ProcessorID pid){
        static_assert(sizeof(hal::ProcessorID) <= sizeof(uint8_t), "You need to update your use of gsbase to support a larger processor ID");
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        currentGsBase &= ~0xfful;
        currentGsBase |= static_cast<uint64_t>(pid) & 0xfful;
        asm volatile("wrgsbase %0" : : "r"(currentGsBase));
    }

    hal::ProcessorID getLogicalProcessorID(){
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        return currentGsBase & 0xff;
    }

    void populateProcessorInfo(acpi::MADT& madt) {
        pinfo = new ProcessorInfo[hal::processorCount()];
        size_t countedAPs = 0; //minus the BSP
        auto bspLapicID = interrupts::getLAPICDomain() -> getID();
        for(auto& entry : madt.entries<acpi::MADT_LAPIC_Entry>()){
            if((entry.flags & 3) == 1){
                //Always ensure the BSP is marked as CPU 0
                if (entry.apicID == bspLapicID) {
                    pinfo[0] = {
                        0,
                        entry.apicID,
                        entry.acpiProcessorID
                    };
                }
                else {
                    countedAPs++;
                    pinfo[countedAPs] = {
                        static_cast<ProcessorID>(countedAPs),
                        entry.apicID,
                        entry.acpiProcessorID
                    };
                }
            }
        }
    }

    const ProcessorInfo &getProcessorInfoForAcpiID(uint8_t acpiID) {
        for (size_t i = 0; i < hal::processorCount(); i++) {
            if (pinfo[i].acpiProcessorID == acpiID) {
                return pinfo[i];
            }
        }
        assertNotReached("Tried to look up processor info for absent ACPI ID");
    }

    const ProcessorInfo &getProcessorInfoForLapicID(uint8_t lapicID) {
        for (size_t i = 0; i < hal::processorCount(); i++) {
            if (pinfo[i].lapicID == lapicID) {
                return pinfo[i];
            }
        }
        assertNotReached("Tried to look up processor info for absent APIC ID");
    }

    const ProcessorInfo &getProcessorInfoForProcessorID(hal::ProcessorID pid) {
        return pinfo[pid];
    }

    extern "C" {
        extern uint8_t trampoline_template_start;
        extern uint8_t trampoline_template_end;
    }

    void initProcessor(ProcessorID pid) {
        auto lapic = interrupts::getLAPICDomain();
        //Just make sure there's nothing trying to be sent already... there really shouldn't be
        klog << "Initializing processor " << pid << "\n";
        lapic -> issueIPISync({interrupts::INIT, true, 0, pid});
        timing::blockingSleep(10);
        lapic -> issueIPISync({interrupts::INIT, false, 0, pid});
        timing::blockingSleep(10);
        klog << "Init IPI sent... now issuing SIPIs\n";
        for (size_t i = 0; i < 2; i++) {
            lapic -> issueIPISync({interrupts::SIPI, false, SMP_TRAMPOLINE_START, pid});
            timing::sleepns(200'000);
        }
    }

    void setupTrampoline() {
        auto trampolineSize = static_cast<uint64_t>(&trampoline_template_end - &trampoline_template_start);
        auto trampolineDestination = early_boot_phys_to_virt(mm::phys_addr(0x1000 * SMP_TRAMPOLINE_START)).as_ptr<uint8_t>();
        memcpy(trampolineDestination, &trampoline_template_start, trampolineSize);
    }

    void smpInit() {
        setupTrampoline();
        for (size_t i = 1; i < hal::processorCount(); i++) {
            initProcessor(static_cast<ProcessorID>(i));
        }
    }
}