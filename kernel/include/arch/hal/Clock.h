//
// Created by Spencer Martin on 12/19/25.
//

#ifndef CROCOS_CLOCKSOURCE_H
#define CROCOS_CLOCKSOURCE_H

#include <stdint.h>
#include <core/FrequencyData.h>

namespace kernel::hal::timing {
    class ClockSource;
    class EventSource;
}

namespace kernel::timing {
    void calibrateClockSource(kernel::hal::timing::ClockSource& knownReference, kernel::hal::timing::ClockSource& toCalibrate);
    void calibrateEventSource(kernel::hal::timing::EventSource& knownReference, kernel::hal::timing::EventSource& toCalibrate);
}

namespace kernel::hal::timing {
    using FrequencyData = Core::FrequencyData;

    using csflags_t = uint8_t;

    constexpr csflags_t CS_FIXED_FREQUENCY = 1 << 0;
    constexpr csflags_t CS_PERCPU = 1 << 1;  // Per-CPU
    constexpr csflags_t CS_KNOWN_STABLE = 1 << 2; //For cases like the LAPIC timer, where the frequency is known to be stable but still needs calibration

    class ClockSource {
    protected:
        FrequencyData _calibrationData;
        uint16_t _quality;

        ClockSource(const char* n, uint64_t m, csflags_t f)
            : name(n), mask(m), flags(f) {}

        virtual void setConversion(FrequencyData data) {
            _calibrationData = data;
        }

        friend void kernel::timing::calibrateClockSource(ClockSource&, ClockSource&);
    public:
        virtual ~ClockSource() = default;

        const char* name;
        const uint64_t mask;      // Counter width mask (0xFFFFFFFFFFFFFFFF for 64-bit)
        const csflags_t flags;

        [[nodiscard]] FrequencyData calibrationData() const {return _calibrationData;}

        // Read raw counter value
        [[nodiscard]] virtual uint64_t read() const = 0;

        // Helper: convert to nanoseconds
        [[nodiscard]] uint64_t readns() const {
            return calibrationData().nanosToTicks(read());
        }

        [[nodiscard]] uint16_t quality() const {return _quality;}

        [[nodiscard]] bool supportsFixedFrequency() const { return flags & CS_FIXED_FREQUENCY; }
        [[nodiscard]] bool hasStableFrequency() const { return flags & (CS_KNOWN_STABLE | CS_FIXED_FREQUENCY); }
        [[nodiscard]] bool isPerCPU() const { return flags & CS_PERCPU; }
        [[nodiscard]] bool isCalibrated() const { return _calibrationData.populated(); }
    };

    using esflags_t = uint8_t;

    constexpr esflags_t ES_FIXED_FREQUENCY = 1 << 0;
    constexpr esflags_t ES_PERCPU = 1 << 1; // Per-CPU (like LAPIC)
    constexpr esflags_t ES_KNOWN_STABLE = 1 << 2; //For cases like the LAPIC timer, where the frequency is known to be stable but still needs calibration
    constexpr esflags_t ES_ONESHOT = 1 << 3;
    constexpr esflags_t ES_PERIODIC = 1 << 4;
    constexpr esflags_t ES_STOPS_IN_SLEEP = 1 << 5;  // Stops in C3/deeper
    constexpr esflags_t ES_TRACKS_INTERMEDIATE_TIME = 1 << 6;  // Is ticksElapsed implemented

    using ClockEventCallback = void (*)();

    class EventSource {
    protected:
        FrequencyData _calibrationData;
        uint16_t _quality;
        ClockEventCallback callback = nullptr;

        virtual void setConversion(FrequencyData data) {
            _calibrationData = data;
        }

        EventSource(const char* n, esflags_t f)
            : name(n), flags(f) {}

        friend void kernel::timing::calibrateEventSource(EventSource&, EventSource&);
    public:
        virtual ~EventSource() = default;

        const char* name;
        const esflags_t flags;

        // Check capabilities
        [[nodiscard]] bool supportsFixedFrequency() const { return flags & ES_FIXED_FREQUENCY; }
        [[nodiscard]] bool hasStableFrequency() const { return flags & (ES_KNOWN_STABLE | ES_FIXED_FREQUENCY); }
        [[nodiscard]] bool supportsOneshot() const { return flags & ES_ONESHOT; }
        [[nodiscard]] bool supportsPeriodic() const { return flags & ES_PERIODIC; }
        [[nodiscard]] bool supportsTicksElapsed() const { return flags & ES_TRACKS_INTERMEDIATE_TIME; }
        [[nodiscard]] bool isPerCPU() const { return flags & ES_PERCPU; }
        [[nodiscard]] bool isCalibrated() const { return _calibrationData.populated(); }

        [[nodiscard]] uint16_t quality() const {return _quality;}

        virtual void armOneshot(uint64_t deltaTicks) = 0;
        virtual void armPeriodic(uint64_t periodTicks) = 0;
        virtual void disarm() = 0;

        [[nodiscard]] FrequencyData calibrationData() const {return _calibrationData;}

        [[nodiscard]] virtual uint64_t ticksElapsed() = 0;
        void registerCallback(ClockEventCallback cb){callback = cb;}
        void unregisterCallback(){callback = nullptr;}
        [[nodiscard]] ClockEventCallback callbackFunction() const {return callback;}
    };
}

#endif