//
// Created by Spencer Martin on 8/24/25.
//

#ifndef CROCOS_APIC_H
#define CROCOS_APIC_H

#include <core/Object.h>
#include <interrupts/interrupts.h>
#include <acpi.h>
#include <mmio/Register.h>

namespace arch::amd64::interrupts{
    using kernel::interrupts::InterruptLineActivationType;
    using kernel::interrupts::platform::InterruptDomain;
    using kernel::interrupts::platform::FreeRoutableDomain;
    using kernel::interrupts::platform::MaskableDomain;
    using kernel::interrupts::platform::ConfigurableActivationTypeDomain;
    using kernel::interrupts::platform::FixedRoutingDomain;
    using kernel::interrupts::platform::EOIDomain;

    CRClass(IOAPIC, public InterruptDomain, public FreeRoutableDomain, public MaskableDomain,
        public ConfigurableActivationTypeDomain){
        uint8_t id;
        volatile uint32_t* mmio_window;
        uint32_t gsi_base;
        [[nodiscard]] uint32_t regRead(uint8_t index) const;
        size_t lineCount;
        UniquePtr<Optional<InterruptLineActivationType>[]> activationTypes;
        void regWrite(uint8_t index, uint32_t value);
    public:
        IOAPIC(uint8_t id, void* mmio_window, uint32_t gsi_base);
        ~IOAPIC() override;
        void setActivationTypeByGSI(uint32_t gsi, InterruptLineActivationType type);
        void setNonmaskable(uint32_t gsi, bool nonmaskable = true);
        [[nodiscard]] size_t getReceiverCount() const override;
        [[nodiscard]] size_t getEmitterCount() const override;
        bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override;
        [[nodiscard]] bool isReceiverMasked(size_t receiver) const override;
        void setReceiverMask(size_t receiver, bool shouldMask) override;
        [[nodiscard]] uint32_t getGSIBase() const;
        void setActivationType(size_t receiver, InterruptLineActivationType type) override;
        [[nodiscard]] Optional<InterruptLineActivationType> getActivationType(size_t receiver) const override;
        void setUninitializedActivationTypes(InterruptLineActivationType type);
    };

    enum IPIType {
        STANDARD,
        INIT,
        SIPI
    };

    enum IPIDestination {
        ALL_CPUS,
        OTHER_CPUS,
        SELF,
        SPECIFIC_LAPIC
    };

    struct IPIRequest {
        IPIType type;
        bool level; //Only for init deassert
        uint8_t vector;
        arch::ProcessorID target;
        IPIDestination destination;
    };

    CRClass(LAPIC, public InterruptDomain, public FixedRoutingDomain, public EOIDomain) {
        Register<uint32_t>* mmio_window;
        Register<uint32_t>& reg(size_t offset);
        friend class LAPICTimer;
        friend class LAPICLocalDeviceRoutingDomain;
    public:
        LAPIC(mm::phys_addr paddr);
        ~LAPIC() override;
        [[nodiscard]] size_t getEmitterFor(size_t receiver) const override;
        void issueEOI() override;
        [[nodiscard]] size_t getReceiverCount() const override;
        [[nodiscard]] size_t getEmitterCount() const override;
        uint32_t getID();
        bool issueIPI(IPIRequest request);
        void issueIPISync(IPIRequest request);
        bool ipiStillSending();
        void waitForIPIToSend();
    };

    uint64_t enableAPICOnAP();
    void setupAPICs(acpi::MADT& madt);
    SharedPtr<LAPIC> getLAPICDomain();
    SharedPtr<IOAPIC> getFirstIOAPIC();
}

#endif //CROCOS_APIC_H