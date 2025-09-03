//
// Created by Spencer Martin on 8/28/25.
//
#include <arch/amd64/timers/PIT.h>
#include <arch/hal/interrupts.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/amd64.h>
#include <arch/amd64/interrupts/APIC.h>

namespace kernel::amd64::timers{
    constexpr uint32_t PIT_FREQUENCY = 1193182;
    constexpr uint32_t PIT_CHANNEL_0 = 0x40;
    constexpr uint32_t PIT_CHANNEL_1 = 0x41;
    constexpr uint32_t PIT_CHANNEL_2 = 0x42;
    constexpr uint32_t PIT_CHANNEL_3 = 0x43;
    constexpr uint32_t PIT_COMMAND_PORT = 0x43;

    using namespace kernel::hal::interrupts;
    CRClass(PITInterruptDomain, public platform::InterruptDomain, public platform::InterruptEmitter){
    public:
        size_t getEmitterCount() override {
            return 1;
        }
    };

    void timerTick(hal::InterruptFrame&) {
        static size_t ticks = 0;
        ticks++;
        if (ticks % 20 == 0)
            klog << "Tick!\n";
        interrupts::lapicIssueEOI();
    }

    void initPIT(){
        auto interruptDomain = make_shared<PITInterruptDomain>();
        topology::registerDomain(interruptDomain);
        auto irqDomain = interrupts::getIRQDomain();
        auto connector = make_shared<platform::AffineConnector>(interruptDomain, irqDomain, 0, 0, 1);
        topology::registerConnector(connector);
        hal::interrupts::managed::registerHandler(managed::InterruptSourceHandle(interruptDomain, 0), make_unique<managed::InterruptHandler>(timerTick));
        {
            interrupts::InterruptDisabler d;
            outb(PIT_COMMAND_PORT, 0x36);
            outb(PIT_CHANNEL_0, 0xff);
            outb(PIT_CHANNEL_0, 0xff);
        }
    }
}