//
// Created by Spencer Martin on 2/18/25.
//

#ifndef CROCOS_MATH_H
#define CROCOS_MATH_H

#include <stdint.h>
#include <stddef.h>

template <typename T>
constexpr T divideAndRoundUp(T numerator, T denominator){
    return (numerator + (denominator - 1)) / denominator;
}

template <typename T>
constexpr T divideAndRoundDown(T numerator, T denominator){
    return numerator / denominator;
}

template <typename T>
constexpr T roundUpToNearestMultiple(T toRound, T divisor){
    return divideAndRoundUp(toRound, divisor) * divisor;
}

template <typename T>
constexpr T roundDownToNearestMultiple(T toRound, T divisor){
    return divideAndRoundDown(toRound, divisor) * divisor;
}

template <typename T>
constexpr T log2floor(T value){
    T log = 0;
    while(value >>= 1) log++;
    return log;
}

constexpr uint64_t log2floor(uint64_t value){
#ifdef __x86_64__
    return static_cast<uint64_t>(63 - __builtin_clzl(value));
#else
    size_t log = 0;
    while(value >>= 1) log++;
    return log;
#endif
}

constexpr __uint128_t log2floor(__uint128_t value){
    if (value >> 64 == 0) {
        return log2floor(static_cast<uint64_t>(value));
    }
    return log2floor(static_cast<uint64_t>(value >> 64)) + 64;
}

constexpr uint32_t log2floor(uint32_t value){
#ifdef __x86_64__
    return static_cast<uint32_t>(63 - __builtin_clz(value));
#else
    size_t log = 0;
    while(value >>= 1) log++;
    return log;
#endif
}

template <typename T>
constexpr T largestPowerOf2Dividing(T value) {
    T result = 1;
    if (value == 0) return 0;
    while ((value & 1) == 0) {
        value >>= 1;
        result <<= 1;
    }
    return result;
}

constexpr size_t gcd(size_t a, size_t b) {
    return b == 0 ? a : gcd(b, a % b);
}

constexpr size_t lcm(size_t a, size_t b) {
    return (a / gcd(a, b)) * b;
}

constexpr size_t max(size_t a) {
    return a;
}

constexpr size_t max(size_t a, size_t b) {
    return a > b ? a : b;
}

template <typename... Rest>
constexpr size_t max(size_t a, size_t b, Rest... rest) {
    return max(max(a, b), rest...);
}

constexpr uint64_t multShift64(uint64_t a, uint64_t mult, uint64_t shift) {
    return static_cast<uint64_t>(static_cast<__uint128_t>(a) * mult) >> shift;
}

#endif //CROCOS_MATH_H
