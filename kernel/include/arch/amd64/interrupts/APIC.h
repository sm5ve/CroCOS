//
// Created by Spencer Martin on 8/24/25.
//

#ifndef CROCOS_APIC_H
#define CROCOS_APIC_H

#include <core/Object.h>
#include <arch/hal/interrupts.h>
#include <acpi.h>

namespace kernel::amd64::interrupts{
    using namespace hal::interrupts::platform;
    CRClass(IOAPIC, public InterruptDomain, public FreeRoutableDomain, public MaskableDomain){
        uint8_t id;
        volatile uint32_t* mmio_window;
        uint32_t gsi_base;
        [[nodiscard]] uint32_t regRead(uint8_t index) const;
        size_t lineCount;
        void regWrite(uint8_t index, uint32_t value);
    public:
        IOAPIC(uint8_t id, void* mmio_window, uint32_t gsi_base);
        ~IOAPIC() override;
        void setActivationType(uint32_t gsi, hal::interrupts::InterruptLineActivationType type);
        void setNonmaskable(uint32_t gsi, bool nonmaskable = true);
        size_t getReceiverCount() override;
        size_t getEmitterCount() override;
        bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override;
        [[nodiscard]] bool isReceiverMasked(size_t receiver) const override;
        void setReceiverMask(size_t receiver, bool shouldMask) override;
        [[nodiscard]] uint32_t getGSIBase() const;
    };

    void setupIOAPICs(acpi::MADT& madt);
}

#endif //CROCOS_APIC_H