//
// Created by Spencer Martin on 4/12/25.
//

#ifndef CROCOS_SMP_H
#define CROCOS_SMP_H

#include "amd64.h"

namespace kernel::amd64::smp{
    struct ProcessorInfo{
        hal::ProcessorID logicalID;
        uint8_t lapicID;
        uint8_t acpiProcessorID;
    };

    void setLogicalProcessorID(hal::ProcessorID pid);
    hal::ProcessorID getLogicalProcessorID();

    const ProcessorInfo& getProcessorInfoForLapicID(uint8_t lapicID);
    const ProcessorInfo& getProcessorInfoForAcpiID(uint8_t acpiID);
}

#endif //CROCOS_SMP_H
