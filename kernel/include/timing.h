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

    hal::timing::ClockSource& getClockSource();      // Current active source
    hal::timing::EventSource& getEventSource(); // Per-CPU event source

    using ClockSourceChangeCallback = void(*)(hal::timing::ClockSource& oldClock, hal::timing::ClockSource& newClock);
    using EventSourceChangeCallback = void(*)(hal::timing::EventSource& oldEventSource, hal::timing::EventSource& newEventSource);

    void registerClockSourceChangeCallback(ClockSourceChangeCallback callback);
    void registerEventSourceChangeCallback(EventSourceChangeCallback callback);

    // TimerQueues.cpp
}

#endif //CROCOS_CLOCKMANAGER_H