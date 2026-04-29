//
// Created by Spencer Martin on 8/26/25.
//

#ifndef CROCOS_IRQDOMAIN_H
#define CROCOS_IRQDOMAIN_H

#include <core/Object.h>
#include <interrupts/interrupts.h>
#include <core/ds/Bimap.h>
#include <core/ds/SmartPointer.h>

namespace arch::amd64::interrupts{
    using kernel::interrupts::platform::InterruptDomain;
    using kernel::interrupts::platform::InterruptEmitter;
    using kernel::interrupts::platform::FixedRoutingDomain;
    using kernel::interrupts::platform::DomainConnector;
    using kernel::interrupts::platform::DomainInputIndex;
    using kernel::interrupts::platform::DomainOutputIndex;

    constexpr size_t ISA_IRQ_COUNT = 16;

    CRClass(IRQDomain, public InterruptDomain, public FixedRoutingDomain){
        size_t surjectiveMapping[ISA_IRQ_COUNT];
        size_t maxMapping;
    public:
        IRQDomain(size_t mapping[ISA_IRQ_COUNT]);
        [[nodiscard]] size_t getReceiverCount() const override;
        [[nodiscard]] size_t getEmitterCount() const override;
        [[nodiscard]] size_t getEmitterFor(size_t receiver) const override;
    };

    CRClass(ExceptionVectorDomain, public InterruptDomain, public InterruptEmitter){
        size_t evc;
    public:
        ExceptionVectorDomain(size_t exceptionVectorCount) : evc(exceptionVectorCount) {}
        [[nodiscard]] size_t getEmitterCount() const override { return evc; }
    };

    class IRQToIOAPICConnector : public DomainConnector{
        Bimap<size_t, size_t> irqToAPICLineMap;
    public:
        IRQToIOAPICConnector(SharedPtr<IRQDomain> irqDomain, SharedPtr<InterruptDomain> ioapic, Bimap<size_t, size_t>&& map);
        Optional<DomainInputIndex> fromOutput(DomainOutputIndex) const override;
        Optional<DomainOutputIndex> fromInput(DomainInputIndex) const override;
    };

    SharedPtr<IRQDomain> getIRQDomain();
}

#endif //CROCOS_IRQDOMAIN_H