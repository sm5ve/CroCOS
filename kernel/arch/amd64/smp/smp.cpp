//
// Created by Spencer Martin on 4/12/25.
//

#include <acpi.h>
#include <init.h>
#include <timing/timing.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/interrupts/APIC.h>
#include <kconfig.h>
#include "../amd64internal.h"

namespace arch::amd64::smp{
    ProcessorInfo* pinfo;

    void setLogicalProcessorID(ProcessorID pid){
        static_assert(sizeof(ProcessorID) <= sizeof(uint8_t), "You need to update your use of gsbase to support a larger processor ID");
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        currentGsBase &= ~0xfful;
        currentGsBase |= static_cast<uint64_t>(pid) & 0xfful;
        asm volatile("wrgsbase %0" : : "r"(currentGsBase));
    }

    ProcessorID getLogicalProcessorID(){
        uint64_t currentGsBase;
        asm volatile("rdgsbase %0" : "=r"(currentGsBase));
        return currentGsBase & 0xff;
    }

    bool populateProcessorInfo() {
        auto& madt = acpi::the<MADT>();
        pinfo = new ProcessorInfo[processorCount()];
        size_t countedAPs = 0; //minus the BSP
        auto bspLapicID = interrupts::getLAPICDomain() -> getID();
        for(auto& entry : madt.entries<MADT_LAPIC_Entry>()){
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
        return true;
    }

    const ProcessorInfo &getProcessorInfoForAcpiID(uint8_t acpiID) {
        for (size_t i = 0; i < processorCount(); i++) {
            if (pinfo[i].acpiProcessorID == acpiID) {
                return pinfo[i];
            }
        }
        assertNotReached("Tried to look up processor info for absent ACPI ID");
    }

    const ProcessorInfo &getProcessorInfoForLapicID(uint8_t lapicID) {
        for (size_t i = 0; i < processorCount(); i++) {
            if (pinfo[i].lapicID == lapicID) {
                return pinfo[i];
            }
        }
        assertNotReached("Tried to look up processor info for absent APIC ID");
    }

    const ProcessorInfo &getProcessorInfoForProcessorID(ProcessorID pid) {
        return pinfo[pid];
    }

    extern "C" {
        extern uint8_t trampoline_template_start;
        extern uint8_t trampoline_template_end;
        extern uint64_t smp_bringup_pml4;
        extern uint64_t smp_bringup_stack;
    }

    using SMPStack = uint8_t[KERNEL_STACK_SIZE];
    SMPStack stacks[128]; //TODO replace temporary hack

    void initProcessor(ProcessorID pid) {
        auto lapic = interrupts::getLAPICDomain();
        //Just make sure there's nothing trying to be sent already... there really shouldn't be
        klog << "Initializing processor " << pid << "\n";
        lapic -> issueIPISync({interrupts::INIT, true, 0, pid});
        timing::blockingSleep(10);
        lapic -> issueIPISync({interrupts::INIT, false, 0, pid});
        timing::blockingSleep(10);
        for (size_t i = 0; i < 2; i++) {
            lapic -> issueIPISync({interrupts::SIPI, false, SMP_TRAMPOLINE_START, pid});
            timing::sleepns(200'000);
        }
    }

    void setupTrampoline() {
        auto trampolineSize = static_cast<uint64_t>(&trampoline_template_end - &trampoline_template_start);
        auto trampolineDestination = early_boot_phys_to_virt(mm::phys_addr(0x1000 * SMP_TRAMPOLINE_START)).as_ptr<uint8_t>();
        asm volatile("mov %%cr3, %0" : "=r"(smp_bringup_pml4));
        klog << "set pml4 to " << (void*)smp_bringup_pml4 << "\n";
        smp_bringup_stack = reinterpret_cast<uint64_t>(&stacks[0]);
        memcpy(trampolineDestination, &trampoline_template_start, trampolineSize);
    }

    bool smpInit() {
        sti();
        setupTrampoline();
        remapIdentity();
        for (size_t i = 1; i < processorCount(); i++) {
            const auto pid = static_cast<ProcessorID>(i);
            initProcessor(pid);
        }
        unmapIdentity();
        return true;
    }
}

using namespace kernel;
extern "C" void smpEntry() {
    kernel::init::kinit(false, KERNEL_INIT_LOG_LEVEL, false);
}