//
// Created by Spencer Martin on 12/30/25.
//
#include <core/FrequencyData.h>
#include <core/math.h>
#include <assert.h>
using namespace Core;

FrequencyData::FrequencyData() {
    freq = 0;
}

FrequencyData::FrequencyData(__uint128_t m) : freq(m) {
    constexpr __uint128_t one = 1;
    period = (one << 127) / (freq >> 1);
}

bool FrequencyData::populated() const { return freq != 0; }

// Convert nanoseconds to ticks
uint64_t FrequencyData::nanosToTicks(uint64_t ns) const {
    return static_cast<uint64_t>((ns * freq) >> 64);
}

// Convert ticks to nanoseconds
uint64_t FrequencyData::ticksToNanos(uint64_t ticks) const {
    return static_cast<uint64_t>((ticks * period) >> 64);
}

// --- Convenience factory methods for creating from frequencies ---

/**
 * Create FrequencyData from a frequency in Hertz (Hz).
 * Example: FrequencyData::fromHz(1000000000) creates data for a 1 GHz clock
 */
FrequencyData FrequencyData::fromHz(uint64_t hz) {
    __uint128_t m = (static_cast<__uint128_t>(hz) << 64) / 1'000'000'000ULL;
    return FrequencyData(m);
}

/**
 * Create FrequencyData from a frequency in Kilohertz (KHz).
 * Example: FrequencyData::fromKHz(1000000) creates data for a 1 GHz clock
 */
FrequencyData FrequencyData::fromKHz(uint64_t khz) {
    __uint128_t m = (static_cast<__uint128_t>(khz) << 64) / 1'000'000ULL;
    return FrequencyData(m);
}

/**
 * Create FrequencyData from a frequency in Megahertz (MHz).
 * Example: FrequencyData::fromMHz(1000) creates data for a 1 GHz clock
 */
FrequencyData FrequencyData::fromMHz(uint64_t mhz) {
    __uint128_t m = (static_cast<__uint128_t>(mhz) << 64) / 1'000ULL;
    return FrequencyData(m);
}

/**
 * Create FrequencyData from a frequency in Gigahertz (GHz).
 * Example: FrequencyData::fromGHz(1) creates data for a 1 GHz clock
 */
FrequencyData FrequencyData::fromGHz(uint64_t ghz) {
    __uint128_t m = static_cast<__uint128_t>(ghz) << 64;
    return FrequencyData(m);
}

// --- Convenience factory methods for creating from periods ---

FrequencyData FrequencyData::fromPeriodFs(uint64_t fs) {
    constexpr __uint128_t oneMillion = 1'000'000ULL;
    __uint128_t m = (oneMillion << 64) / fs;
    return FrequencyData(m);
}

/**
 * Create FrequencyData from a period in nanoseconds.
 * Example: FrequencyData::fromPeriodNs(1) creates data for a 1 GHz clock
 */
FrequencyData FrequencyData::fromPeriodNs(uint64_t ns) {
    constexpr __uint128_t one = 1;
    __uint128_t m = (one << 64) / ns;
    return FrequencyData(m);
}

/**
 * Create FrequencyData from a period in microseconds.
 * Example: FrequencyData::fromPeriodUs(1000) creates data for a 1 KHz clock
 */
FrequencyData FrequencyData::fromPeriodUs(uint64_t us) {
    return fromPeriodNs(us * 1'000ULL);
}

/**
 * Create FrequencyData from a period in milliseconds.
 * Example: FrequencyData::fromPeriodMs(1000) creates data for a 1 Hz clock
 */
FrequencyData FrequencyData::fromPeriodMs(uint64_t ms) {
    return fromPeriodNs(ms * 1'000'000ULL);
}

/**
 * Create FrequencyData from a period in seconds.
 * Example: FrequencyData::fromPeriodSeconds(1) creates data for a 1 Hz clock
 */
FrequencyData FrequencyData::fromPeriodSeconds(uint64_t s) {
    return fromPeriodNs(s * 1'000'000'000ULL);
}

FrequencyData FrequencyData::scaledFrequency(uint64_t num, uint64_t denom) const{
    assert(log2floor(num) + log2floor(freq) < 126, "Calibration might overflow");
    return FrequencyData((freq * num) / denom);
}

// Pretty printing support
PrintStream& operator<<(PrintStream& ps, const FrequencyData& fd) {
    if (!fd.populated()) {
        return ps << "FrequencyData{uncalibrated}";
    }

    // Convert freq back to frequency in Hz
    // freq = frequency_ghz * 2^64, so frequency_hz = (freq * 1e9) >> 64
    __uint128_t freq_hz_scaled = fd.freq * 1'000'000'000ULL;
    uint64_t freq_hz = static_cast<uint64_t>(freq_hz_scaled >> 64);

    // Choose the most appropriate unit
    if (freq_hz >= 1'000'000'000ULL) {
        // Display in GHz
        uint64_t ghz_int = freq_hz / 1'000'000'000ULL;
        uint64_t ghz_frac = (freq_hz % 1'000'000'000ULL) / 1'000'000ULL; // 3 decimal places
        ps << ghz_int << ".";
        // Pad with zeros if needed
        if (ghz_frac < 100) ps << "0";
        if (ghz_frac < 10) ps << "0";
        ps << ghz_frac << " GHz";
    } else if (freq_hz >= 1'000'000ULL) {
        // Display in MHz
        uint64_t mhz_int = freq_hz / 1'000'000ULL;
        uint64_t mhz_frac = (freq_hz % 1'000'000ULL) / 1'000ULL; // 3 decimal places
        ps << mhz_int << ".";
        if (mhz_frac < 100) ps << "0";
        if (mhz_frac < 10) ps << "0";
        ps << mhz_frac << " MHz";
    } else if (freq_hz >= 1'000ULL) {
        // Display in KHz
        uint64_t khz_int = freq_hz / 1'000ULL;
        uint64_t khz_frac = freq_hz % 1'000ULL;
        ps << khz_int << ".";
        if (khz_frac < 100) ps << "0";
        if (khz_frac < 10) ps << "0";
        ps << khz_frac << " KHz";
    } else {
        // Display in Hz
        ps << freq_hz << " Hz";
    }

    return ps;
}