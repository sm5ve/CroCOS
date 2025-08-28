//
// Created by Spencer Martin on 8/26/25.
//

#ifndef CROCOS_IRQDOMAIN_H
#define CROCOS_IRQDOMAIN_H

#include <core/Object.h>
#include <arch/hal/interrupts.h>
#include <core/ds/Bimap.h>

namespace kernel::amd64::interrupts{
    using namespace kernel::hal::interrupts::platform;
    CRClass(IRQDomain, public InterruptDomain, public FixedRoutingDomain){
        size_t surjectiveMapping[16];
        size_t maxMapping;
    public:
        IRQDomain(size_t mapping[16]);
        size_t getReceiverCount() override;
        size_t getEmitterCount() override;
        size_t getEmitterFor(size_t receiver) const override;
    };

    class IRQToIOAPICConnector : public DomainConnector{
        Bimap<size_t, size_t> irqToAPICLineMap;
    public:
        IRQToIOAPICConnector(SharedPtr<IRQDomain> irqDomain, SharedPtr<InterruptDomain> ioapic, Bimap<size_t, size_t>&& map);
        Optional<DomainInputIndex> fromOutput(DomainOutputIndex) override;
        Optional<DomainOutputIndex> fromInput(DomainInputIndex) override;
    };
}

#endif //CROCOS_IRQDOMAIN_H