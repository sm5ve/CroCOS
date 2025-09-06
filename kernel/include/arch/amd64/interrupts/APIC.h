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
    CRClass(IOAPIC, public InterruptDomain, public FreeRoutableDomain, public MaskableDomain,
        public ConfigurableActivationTypeDomain){
        uint8_t id;
        volatile uint32_t* mmio_window;
        uint32_t gsi_base;
        [[nodiscard]] uint32_t regRead(uint8_t index) const;
        size_t lineCount;
        UniquePtr<Optional<hal::interrupts::InterruptLineActivationType>[]> activationTypes;
        void regWrite(uint8_t index, uint32_t value);
    public:
        IOAPIC(uint8_t id, void* mmio_window, uint32_t gsi_base);
        ~IOAPIC() override;
        void setActivationTypeByGSI(uint32_t gsi, hal::interrupts::InterruptLineActivationType type);
        void setNonmaskable(uint32_t gsi, bool nonmaskable = true);
        size_t getReceiverCount() override;
        size_t getEmitterCount() override;
        bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override;
        [[nodiscard]] bool isReceiverMasked(size_t receiver) const override;
        void setReceiverMask(size_t receiver, bool shouldMask) override;
        [[nodiscard]] uint32_t getGSIBase() const;
        void setActivationType(size_t receiver, hal::interrupts::InterruptLineActivationType type) override;
        [[nodiscard]] Optional<hal::interrupts::InterruptLineActivationType> getActivationType(size_t receiver) const override;
        void setUninitializedActivationTypes(hal::interrupts::InterruptLineActivationType type);
    };

    CRClass(LAPIC, public InterruptDomain, public FixedRoutingDomain, public EOIDomain) {
        volatile uint32_t* mmio_window;
        volatile uint32_t& reg(size_t offset);
    public:
        LAPIC(mm::phys_addr paddr);
        ~LAPIC() override;
        [[nodiscard]] size_t getEmitterFor(size_t receiver) const override;
        void issueEOI(InterruptFrame& iframe) override;
        size_t getReceiverCount() override;
        size_t getEmitterCount() override;
    };

    void setupAPICs(acpi::MADT& madt);
    SharedPtr<LAPIC> getLAPICDomain();
}

#endif //CROCOS_APIC_H