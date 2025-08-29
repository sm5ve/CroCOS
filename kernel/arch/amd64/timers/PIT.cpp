//
// Created by Spencer Martin on 8/28/25.
//
#include <arch/amd64/timers/PIT.h>
#include <arch/hal/interrupts.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>

namespace kernel::amd64::timers{
    using namespace kernel::hal::interrupts;
    CRClass(PITInterruptDomain, public platform::InterruptDomain, public platform::InterruptEmitter){
    public:
        size_t getEmitterCount() override {
            return 1;
        }
    };

    void initPIT(){
        auto interruptDomain = make_shared<PITInterruptDomain>();
        topology::registerDomain(interruptDomain);
        auto irqDomain = interrupts::getIRQDomain();
        auto connector = make_shared<platform::AffineConnector>(interruptDomain, irqDomain, 0);
        topology::registerConnector(connector);
    }
}