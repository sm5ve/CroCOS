//
// Created by Spencer Martin on 12/30/25.
//

#ifndef CROCOS_FREQUENCYDATA_H
#define CROCOS_FREQUENCYDATA_H

#include <stdint.h>
#include "PrintStream.h"

namespace Core {

    /**
     * FrequencyData represents timing calibration data for clock sources.
     *
     * Internally stores:
     * - freq: frequency in units of (ticks/ns) * 2^64 = GHz * 2^64
     * - period: period in units of (ns/tick) * 2^64 = (1/GHz) * 2^64
     *
     * This allows for efficient fixed-point conversion between ticks and nanoseconds.
     */
    struct FrequencyData {
        __uint128_t freq;       // Frequency: ticks per nanosecond, scaled by 2^64
        __uint128_t period;     // Period: nanoseconds per tick, scaled by 2^64
    private:
        FrequencyData(__uint128_t m);
    public:
        FrequencyData();

        bool populated() const;

        // Convert nanoseconds to ticks
        uint64_t ticksToNanos(uint64_t ns) const;

        // Convert ticks to nanoseconds
        uint64_t nanosToTicks(uint64_t ticks) const;

        // --- Convenience factory methods for creating from frequencies ---

        /**
         * Create FrequencyData from a frequency in Hertz (Hz).
         * Example: FrequencyData::fromHz(1000000000) creates data for a 1 GHz clock
         */
        static FrequencyData fromHz(uint64_t hz);

        /**
         * Create FrequencyData from a frequency in Kilohertz (KHz).
         * Example: FrequencyData::fromKHz(1000000) creates data for a 1 GHz clock
         */
        static FrequencyData fromKHz(uint64_t khz);

        /**
         * Create FrequencyData from a frequency in Megahertz (MHz).
         * Example: FrequencyData::fromMHz(1000) creates data for a 1 GHz clock
         */
        static FrequencyData fromMHz(uint64_t mhz);

        /**
         * Create FrequencyData from a frequency in Gigahertz (GHz).
         * Example: FrequencyData::fromGHz(1) creates data for a 1 GHz clock
         */
        static FrequencyData fromGHz(uint64_t ghz);

        // --- Convenience factory methods for creating from periods ---

        /**
         * Create FrequencyData from a period in nanoseconds.
         * Example: FrequencyData::fromPeriodNs(1) creates data for a 1 GHz clock
         */
        static FrequencyData fromPeriodNs(uint64_t ns);

        /**
         * Create FrequencyData from a period in microseconds.
         * Example: FrequencyData::fromPeriodUs(1000) creates data for a 1 KHz clock
         */
        static FrequencyData fromPeriodUs(uint64_t us);

        /**
         * Create FrequencyData from a period in milliseconds.
         * Example: FrequencyData::fromPeriodMs(1000) creates data for a 1 Hz clock
         */
        static FrequencyData fromPeriodMs(uint64_t ms);

        /**
         * Create FrequencyData from a period in seconds.
         * Example: FrequencyData::fromPeriodSeconds(1) creates data for a 1 Hz clock
         */
        static FrequencyData fromPeriodSeconds(uint64_t s);

        FrequencyData scaledFrequency(uint64_t num, uint64_t denom) const;
    };
}

// Pretty printing support
Core::PrintStream& operator<<(Core::PrintStream& ps, const Core::FrequencyData& fd);

#endif //CROCOS_FREQUENCYDATA_H