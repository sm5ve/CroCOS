//
// Created by Spencer Martin on 12/22/25.
//

#ifndef CROCOS_CLOCKMANAGER_H
#define CROCOS_CLOCKMANAGER_H

#include <timing/Clock.h>

namespace kernel::timing {
    // ClockManager.cpp
    // Initialization
    void registerClockSource(ClockSource& source);
    void registerEventSource(EventSource& source);

    //Convenience method for calibrating and selecting sources
    bool initialize();

    void dumpTimerInfo();

    uint64_t monoTimens();  // Monotonic time in nanoseconds
    uint64_t monoTimems();  // Convenience: monotonic time in milliseconds

    ClockSource& getClockSource(); // Current active source
    EventSource& getEventSource(); // Per-CPU event source

    using ClockSourceChangeCallback = void(*)(ClockSource& oldClock, ClockSource& newClock);
    using EventSourceChangeCallback = void(*)(EventSource& oldEventSource, EventSource& newEventSource);

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

    QueuedEventHandle enqueueEvent(TimerEventCallback&&, uint64_t preferredDelayMs, uint64_t lateTolerance = 5, uint64_t earlyTolerance = 0);
    bool cancelEvent(QueuedEventHandle handle);

    //BlockingSleep.cpp

    void blockingSleep(uint64_t ms);
    void sleepns(uint64_t ns);
}

#endif //CROCOS_CLOCKMANAGER_H