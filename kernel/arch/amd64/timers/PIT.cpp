//
// Created by Spencer Martin on 8/28/25.
//
#include <timing.h>
#include <arch/amd64/timers/PIT.h>
#include <arch/hal/interrupts.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/amd64.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/hal/Clock.h>
#include <core/atomic.h>

namespace kernel::amd64::timers{
    constexpr uint32_t PIT_FREQUENCY = 1193182; //In Hz
    constexpr uint32_t PIT_CHANNEL_0 = 0x40;
    constexpr uint32_t PIT_CHANNEL_1 = 0x41;
    constexpr uint32_t PIT_CHANNEL_2 = 0x42;
    constexpr uint32_t PIT_COMMAND_PORT = 0x43;

    using namespace hal::timing;

    using namespace kernel::hal::interrupts;
    CRClass(PITInterruptDomain, public platform::InterruptDomain, public platform::InterruptEmitter){
    public:
        size_t getEmitterCount() override {
            return 1;
        }
    };

    void callPITEventCallback(hal::InterruptFrame& frame);

    class PITEventSource : public EventSource{
        static constexpr auto PIT_FLAGS = ES_FIXED_FREQUENCY | ES_KNOWN_STABLE | ES_ONESHOT | ES_PERIODIC | ES_TRACKS_INTERMEDIATE_TIME;

        enum PITState {
            UNINITIALIZED,
            ONESHOT,
            PERIODIC
        };

        uint64_t reloadValue = 0;

        PITState state = UNINITIALIZED;

        void ensureState(PITState target) {
            assert(target != UNINITIALIZED, "Cannot de-initialize PIT");
            if (target == state) return;
            if (target == ONESHOT) {
                outb(PIT_COMMAND_PORT, 0x30);
            }
            else {
                outb(PIT_COMMAND_PORT, 0x34);
            }
            state = target;
        }

        void setReload(uint64_t value) {
            reloadValue = value;
            if (value == 0x10000) {
                outb(PIT_CHANNEL_0, 0);
                outb(PIT_CHANNEL_0, 0);
            }
            else {
                assert(value < 0x10000, "Cannot set PIT reload value greater than 65536");
                outb(PIT_CHANNEL_0, static_cast<uint8_t>(value & 0xff));
                outb(PIT_CHANNEL_0, static_cast<uint8_t>(value >> 8));
            }
        }

        Spinlock pitLock;

        static SharedPtr<PITInterruptDomain> setupPITHardware() {
            static bool initialized = false;
            assert(!initialized, "PIT already initialized");
            auto interruptDomain = make_shared<PITInterruptDomain>();
            topology::registerDomain(interruptDomain);
            auto irqDomain = interrupts::getIRQDomain();
            auto connector = make_shared<platform::AffineConnector>(interruptDomain, irqDomain, 0, 0, 1);
            topology::registerConnector(connector);
            initialized = true;
            return interruptDomain;
        }

        friend void callPITEventCallback(hal::InterruptFrame& frame);
    public:
        PITEventSource() : EventSource("PIT", PIT_FLAGS) {
            _quality = 100;
            _calibrationData = FrequencyData::fromHz(PIT_FREQUENCY);
            auto interruptDomain = setupPITHardware();

            managed::registerHandler(managed::InterruptSourceHandle(interruptDomain, 0), make_unique<managed::InterruptHandler>(callPITEventCallback));
        }

        void armOneshot(uint64_t deltaTicks) override {
            LockGuard guard(pitLock);
            hal::InterruptDisabler disabler;
            ensureState(ONESHOT);
            setReload(deltaTicks);
        }

        [[nodiscard]] uint64_t maxOneshotDelay() const override {
            return 0x10000;
        }

        [[nodiscard]] uint64_t maxPeriod() const override {
            return 0x10000;
        }

        void armPeriodic(uint64_t periodTicks) override {
            LockGuard guard(pitLock);
            hal::InterruptDisabler disabler;
            ensureState(PERIODIC);
            setReload(periodTicks);
        }

        void disarm() override {
            LockGuard guard(pitLock);
            hal::InterruptDisabler disabler;
            assertUnimplemented("PIT Disarm");
        }

        uint64_t ticksElapsed() override {
            LockGuard guard(pitLock);
            hal::InterruptDisabler disabler;
            outb(PIT_COMMAND_PORT, 0x00); //Read latched count
            uint64_t out = 0;
            out = inb(PIT_CHANNEL_0) & 0xff;
            out |= static_cast<uint64_t>(inb(PIT_CHANNEL_0) & 0xff) << 8;
            if (out == 0) {
                out = 0x10000;
            }
            return reloadValue - out;
        }
    };

    PITEventSource pitEventSource;

    void callPITEventCallback(hal::InterruptFrame& frame) {
        (void)frame;
        if (pitEventSource.callback != nullptr)
            pitEventSource.callback();
    }

    void initPIT(){
        new(&pitEventSource) PITEventSource();
        timing::registerEventSource(pitEventSource);
    }
}
