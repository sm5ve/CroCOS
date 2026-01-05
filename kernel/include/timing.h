//
// Created by Spencer Martin on 12/22/25.
//

#ifndef CROCOS_CLOCKMANAGER_H
#define CROCOS_CLOCKMANAGER_H

#include <arch/hal/Clock.h>

namespace kernel::timing {
    // ClockManager.cpp
    // Initialization
    void registerClockSource(hal::timing::ClockSource& source);
    void registerEventSource(hal::timing::EventSource& source);

    //Convenience method for calibrating and selecting sources
    void initialize();

    void dumpTimerInfo();

    uint64_t monoTimens();  // Monotonic time in nanoseconds
    uint64_t monoTimems();  // Convenience: monotonic time in milliseconds

    hal::timing::ClockSource& getClockSource(); // Current active source
    hal::timing::EventSource& getEventSource(); // Per-CPU event source

    using ClockSourceChangeCallback = void(*)(hal::timing::ClockSource& oldClock, hal::timing::ClockSource& newClock);
    using EventSourceChangeCallback = void(*)(hal::timing::EventSource& oldEventSource, hal::timing::EventSource& newEventSource);

    void registerClockSourceChangeCallback(ClockSourceChangeCallback callback);
    void registerEventSourceChangeCallback(EventSourceChangeCallback callback);

    class Stopwatch {
        uint64_t start;

    public:
        Stopwatch();

        [[nodiscard]] uint64_t elapsedNs() const;
        [[nodiscard]] uint64_t elapsedUs() const;
        [[nodiscard]] uint64_t elapsedMs() const;

        void reset();
        [[nodiscard]] uint64_t lap();
    };

    // TimerQueues.cpp
    using TimerEventCallback = Function<void()>;

    struct QueuedEventHandle {
        uint64_t id;

        bool operator==(const QueuedEventHandle& other) const { return id == other.id; }
    };

    constexpr QueuedEventHandle EXPIRED_EVENT{static_cast<uint64_t>(-1)};

    void test();
}

#endif //CROCOS_CLOCKMANAGER_H