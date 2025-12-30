//
// Created by Spencer Martin on 12/22/25.
//

#include <timing.h>
#include <kernel.h>
#include <arch/hal/hal.h>
#include <core/ds/Vector.h>

using namespace kernel::hal::timing;

namespace kernel::timing {
    using ClockSourceVec = Vector<ClockSource*>;
    using EventSourceVec = Vector<EventSource*>;
    WITH_GLOBAL_CONSTRUCTOR(ClockSourceVec, clockSources);
    WITH_GLOBAL_CONSTRUCTOR(EventSourceVec, eventSources);

    ClockSource* watchdogClockSource;
    ClockSource* bestClockSource;

    EventSource* bestEventSource;

    void registerClockSource(ClockSource& source) {
        clockSources.push(&source);
    }

    void registerEventSource(EventSource& source) {
        eventSources.push(&source);
    }

    ClockSource* findBootstrapClock() {
        //If clockSources is empty, we could reasonably use an extra event source to simulate a clock source
        ClockSource* best = nullptr;
        for (auto* cs : clockSources) {
            if (cs -> isCalibrated() && cs -> hasStableFrequency()) {
                if (best == nullptr || cs -> quality() > best -> quality()) {
                    best = cs;
                }
            }
        }
        return best;
    }

    ClockSource* findBestWatchdogClock() {
        ClockSource* best = nullptr;
        for (auto* cs : clockSources) {
            if (cs -> hasStableFrequency()) {
                if (best == nullptr || cs -> quality() > best -> quality()) {
                    best = cs;
                }
            }
        }
        assert(best != nullptr, "No stable clock source found");
        return best;
    }

#define TIMER_PAST_MINIMUM(t) (timerPastMinimum(t ## Val, t ## MinTicks, t ## TimerInitTicks, t ## Overflows))

    inline bool timerPastMinimum(uint64_t val, uint64_t timerMinTicks, uint64_t timerInitTicks, bool overflows) {
        return (val >= timerMinTicks) && (val < timerInitTicks || !overflows);
    }

    struct TimerComparisonData {
        uint64_t aDelta;
        uint64_t bDelta;
    };

    FrequencyData computeFrequencyData(const TimerComparisonData comparisonData, const FrequencyData &knownCalibration) {
        return knownCalibration.scaledFrequency(comparisonData.bDelta, comparisonData.aDelta);
    }

    TimerComparisonData compareTimerTicks(ClockSource& a, ClockSource& b, uint64_t minTicks) {
        hal::InterruptDisabler disabler;
        assert(minTicks < (a.mask >> 1), "minTicks too large, risk of double wrap");
        assert(minTicks < (b.mask >> 1), "minTicks too large, risk of double wrap");
        const uint64_t aTimerInitTicks = a.read();
        const uint64_t bTimerInitTicks = b.read();
        const uint64_t aMinTicks = (aTimerInitTicks + minTicks) & a.mask;
        const uint64_t bMinTicks = (bTimerInitTicks + minTicks) & b.mask;
        const bool aOverflows = aMinTicks < aTimerInitTicks;
        const bool bOverflows = bMinTicks < bTimerInitTicks;
        uint64_t aVal = 0;
        uint64_t bVal = 0;
        do {
            aVal = a.read();
            bVal = b.read();
            asm volatile("pause" ::: "memory");
        } while (!(TIMER_PAST_MINIMUM(a) && TIMER_PAST_MINIMUM(b)));
        const auto aDelta = (aVal - aTimerInitTicks) & a.mask;
        const auto bDelta = (bVal - bTimerInitTicks) & b.mask;
        return {aDelta, bDelta};
    }

    constexpr uint64_t calibrationPrecision = 100000;

    void calibrateClockSource(ClockSource& knownReference, ClockSource& toCalibrate) {
        assert(knownReference.hasStableFrequency(), "Can't calibrate off of unstable clock source");
        assert(knownReference.isCalibrated(), "Can't calibrate off of uncalibrated clock source");
        auto comparisonData = compareTimerTicks(knownReference, toCalibrate, calibrationPrecision);
        auto newCalibration = computeFrequencyData(comparisonData, knownReference.calibrationData());
        toCalibrate.setConversion(newCalibration);
    }

    void initializeWatchdogClock() {
        auto bootstrapClockSource = findBootstrapClock();
        assert(watchdogClockSource != nullptr, "No bootstrap clock source found");
        watchdogClockSource = findBestWatchdogClock();
        if (!watchdogClockSource->isCalibrated()) {
            calibrateClockSource(*bootstrapClockSource, *watchdogClockSource);
        }
    }

    void initializeBestClockSource() {
        ClockSource* best = nullptr;
        for (auto* cs : clockSources) {
            if (best == nullptr || cs -> quality() > best -> quality()) {
                best = cs;
            }
        }
        bestClockSource = best;
        assert(bestClockSource != nullptr, "No clock source found");
        if (!bestClockSource->isCalibrated()) {
            calibrateClockSource(*watchdogClockSource, *bestClockSource);
        }
    }

    void setupWatchdogClockEvent() {
        if (!bestClockSource -> hasStableFrequency()) {
            //TODO
        }
    }

    void initializeEventSource() {
        EventSource* best = nullptr;
        for (auto* es : eventSources) {
            if (best == nullptr || (es -> quality() > best -> quality()) || (es -> isPerCPU() && !best -> isPerCPU())) {
                best = es;
            }
        }
        bestEventSource = best;
        assert(bestEventSource != nullptr, "No event source found");
        if (!bestEventSource -> isCalibrated()) {
            //TODO implement event source calibration
        }
    }

    void initialize() {
        //initializeWatchdogClock();
        //initializeBestClockSource();
        initializeEventSource();
        //setupWatchdogClockEvent();
    }

    ClockSource& getClockSource(){
        return *bestClockSource;
    }

    EventSource& getEventSource(){
        return *bestEventSource;
    }
}