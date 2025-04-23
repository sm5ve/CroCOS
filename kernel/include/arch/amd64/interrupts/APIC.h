//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_APIC_H
#define CROCOS_APIC_H

#include <arch/hal/interrupts.h>
#include <acpi.h>
#include <arch/amd64/amd64.h>

using namespace kernel::hal::interrupts;

namespace kernel::amd64::interrupts{
    void buildApicTopology(acpi::MADT& madt);

    namespace Lapic {
        void initializeLapic();
    };

    class IOapic{
    private:
        uint8_t apicID;
        void* mmio_base;
        uint32_t gsi_base;
    public:
        IOapic(uint8_t id, void* mmio_window, uint32_t gsi_base);

    };
}

#endif //CROCOS_APIC_H
