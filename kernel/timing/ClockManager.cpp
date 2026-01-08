//
// Created by Spencer Martin on 12/22/25.
//

#include <timing.h>
#include <kernel.h>
#include <arch/hal/hal.h>
#include <core/atomic.h>
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
        assert(bootstrapClockSource != nullptr, "No bootstrap clock source found");
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

    //TODO figure out a way to gracefully recalibrate an event source if it's running a touch faster than
    //we expect and the timer queue spams the event source
    void calibrateECEventSource(EventSource& evt) {
        assert(evt.supportsTicksElapsed(), "Event source must track ticks elapsed");
        //Weak assumption to simplify calibration
        assert(evt.maxOneshotDelay() > 4 * calibrationPrecision, "Event source must be able to track at least (4 * calibrationPrecision) ticks");
        auto& clock = getClockSource();
        uint64_t csTotalElapsedTicks = 0;
        uint64_t evtTotalElapsedTicks = 0;
        for (auto i = 0; i < 10; i++) {
            evt.armOneshot(evt.maxOneshotDelay()); //Assumed to be expensive relative to clock.read()
            uint64_t firstRead = clock.read();
            while (true) {
                uint64_t secondRead = clock.read();
                uint64_t evtTicks = evt.ticksElapsed();
                if ((((secondRead - firstRead) & clock.mask) > calibrationPrecision) && (evtTicks > calibrationPrecision)) {
                    break;
                }
                if (evt.maxOneshotDelay() - evtTicks < calibrationPrecision) {
                    klog << "Event source " << evt.name << " ticks significantly faster than main clock source, calibration is not as precise as desired\n";
                    break;
                }
                asm volatile("pause");
            }
            uint64_t csElapsedTicks = (clock.read() - firstRead) & clock.mask;
            uint64_t evtElapsedTicks = evt.ticksElapsed();
            evt.disarm();
            //Discard the first sample in case there's any warm-up delay that skews results. This is a phenomenon I'm observing in QEMU
            if (i > 0) {
                csTotalElapsedTicks += csElapsedTicks;
                evtTotalElapsedTicks += evtElapsedTicks;
            }
        }
        auto evtCalibration = clock.calibrationData().scaledFrequency(evtTotalElapsedTicks, csTotalElapsedTicks);
        evt.setConversion(evtCalibration);
        klog << "Calibrated event source " << evt.name << " against clock source " << clock.name << " to " << evtCalibration << "\n";
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
            if (bestEventSource -> supportsTicksElapsed()) {
                Stopwatch watch;
                calibrateECEventSource(*bestEventSource);
                klog << "Event source calibration took " << watch.elapsedUs() << " microseconds\n";
            }
            else {
                assertUnimplemented("Event source calibration");
            }
            //TODO implement event source calibration
        }
    }

    Atomic<uint64_t> lastReadCSTimestamp = 0;

    void initTimerQueues();

    void initialize() {
        initializeWatchdogClock();
        initializeBestClockSource();
        initializeEventSource();
        lastReadCSTimestamp = getClockSource().read();
        initTimerQueues();
    }

    ClockSource& getClockSource(){
        return *bestClockSource;
    }

    EventSource& getEventSource(){
        return *bestEventSource;
    }

    Atomic<uint64_t> monotimestamp = 0;

    uint64_t monoTimens() {
        uint64_t oldTime = 0;
        uint64_t newTime = 0;
        do {
            oldTime = lastReadCSTimestamp.load(RELAXED);
            newTime = getClockSource().read();
        } while (!lastReadCSTimestamp.compare_exchange(oldTime, newTime, RELAXED));
        const auto delta = (newTime - oldTime) & getClockSource().mask;
        const auto deltaNs = getClockSource().calibrationData().ticksToNanos(delta);
        return monotimestamp.add_fetch(deltaNs, RELAXED);
    }

    uint64_t monoTimems() {
        return monoTimens() / 1'000'000;
    }

    Stopwatch::Stopwatch() {
        start = monoTimens();
    }

    uint64_t Stopwatch::elapsedMs() const {
        return (monoTimens() - start) / 1'000'000;
    }

    uint64_t Stopwatch::elapsedUs() const {
        return (monoTimens() - start) / 1'000;
    }

    uint64_t Stopwatch::elapsedNs() const {
        return monoTimens() - start;
    }

    void Stopwatch::reset() {
        start = monoTimens();
    }

    uint64_t Stopwatch::lap() {
        uint64_t prev = start;
        start = monoTimens();
        return start - prev;
    }
}