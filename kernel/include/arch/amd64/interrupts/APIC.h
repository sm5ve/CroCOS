//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_APIC_H
#define CROCOS_APIC_H

#include <arch/hal/interrupts.h>
#include <acpi.h>
#include <arch/amd64/amd64.h>

using namespace kernel::hal::interrupts;
using namespace hardware;

namespace kernel::amd64::interrupts{
    enum BusType : uint8_t {
        ISA = 0,
        PCI = 1
    };

    void buildApicTopology(acpi::MADT& madt);
    InterruptReceiver getInterruptReceiverForIRQ(uint8_t irqno, BusType bt);

    class Lapic : public IInterruptController, ITerminalController{
    public:
        Lapic();
    };

    class IOapic : public IInterruptController, ITerminalController{
    private:
        Tuple<VectorIndex, InterruptCPUAffinity>* mappings;
        size_t redirectionTableSize;
        uint8_t apicID;
        uint32_t volatile* mmio_base;
        uint32_t gsi_base;

        uint32_t regRead(uint8_t index);
        void regWrite(uint8_t index, uint32_t value);
        Optional<uint32_t> indexForInterruptReceiver(InterruptReceiver r);
    public:
        IOapic(uint8_t id, void* mmio_window, uint32_t gsi_base);
        virtual InterruptControllerFeature getFeatureSet() override;
        virtual bool setInterruptMaskState(InterruptReceiver receiver, bool) override;
        virtual bool getInterruptMaskState(InterruptReceiver receiver) override;
        virtual Optional<Tuple<VectorIndex, Optional<InterruptCPUAffinity>>> getMapping(InterruptReceiver r) override;
        virtual bool setMapping(InterruptReceiver r, VectorIndex, Optional<InterruptCPUAffinity>) override;

        InterruptReceiver getReceiverForGSI(uint32_t gsi);
        void setActivationMode(uint32_t gsi, ActivationType at);
        void setNonmaskable(uint32_t gsi);
        uint32_t getGSIBase();
        size_t getRedirectionTableSize();
    };
}

#endif //CROCOS_APIC_H
