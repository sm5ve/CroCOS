//
// Created by Spencer Martin on 4/12/25.
//

#ifndef CROCOS_SMP_H
#define CROCOS_SMP_H

#include <arch/amd64/amd64.h>
#include <arch.h>
#include <acpi.h>

using namespace kernel::acpi;
namespace arch::amd64::smp{
    struct ProcessorInfo{
        ProcessorID logicalID;
        uint8_t lapicID;
        uint8_t acpiProcessorID;
    };

    void setLogicalProcessorID(ProcessorID pid);
    ProcessorID getLogicalProcessorID();

    const ProcessorInfo& getProcessorInfoForLapicID(uint8_t lapicID);
    const ProcessorInfo& getProcessorInfoForAcpiID(uint8_t acpiID);
    const ProcessorInfo& getProcessorInfoForProcessorID(ProcessorID pid);

    void populateProcessorInfo(MADT& madt);

    bool smpInit();
}

#endif //CROCOS_SMP_H
