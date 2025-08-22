//
// Created by Spencer Martin on 8/22/25.
//

#ifndef CROCOS_DBGSTDDEF_H
#define CROCOS_DBGSTDDEF_H

#include <stddef.h>

struct DebugSizeT {
    size_t value;

    // Constructors
    constexpr DebugSizeT() : value(0) {}
    constexpr DebugSizeT(size_t v) : value(v) {}

    // Conversion back to size_t
    constexpr operator size_t() const { return value; }

    // Assignment
    DebugSizeT& operator=(size_t v) {
        value = v;
        return *this;
    }

    // Pre-increment
    DebugSizeT& operator++() {
        // set breakpoint here
        ++value;
        return *this;
    }

    // Post-increment
    DebugSizeT operator++(int) {
        // set breakpoint here
        DebugSizeT tmp = *this;
        ++value;
        return tmp;
    }

    // Pre-decrement
    DebugSizeT& operator--() {
        // set breakpoint here
        --value;
        return *this;
    }

    // Post-decrement
    DebugSizeT operator--(int) {
        // set breakpoint here
        DebugSizeT tmp = *this;
        --value;
        return tmp;
    }

    operator size_t(){
        return value;
    }

    // Arithmetic
    DebugSizeT operator+(const DebugSizeT& other) const { return value + other.value; }
    DebugSizeT operator-(const DebugSizeT& other) const { return value - other.value; }
    DebugSizeT operator*(const DebugSizeT& other) const { return value * other.value; }
    DebugSizeT operator/(const DebugSizeT& other) const { return value / other.value; }

    DebugSizeT operator+(const size_t other) const { return value + other; }
    DebugSizeT operator-(const size_t other) const { return value - other; }
    DebugSizeT operator*(const size_t other) const { return value * other; }
    DebugSizeT operator/(const size_t other) const { return value / other; }

    DebugSizeT& operator+=(const DebugSizeT& other) { value += other.value; return *this; }
    DebugSizeT& operator-=(const DebugSizeT& other) { value -= other.value; return *this; }
    DebugSizeT& operator*=(const DebugSizeT& other) { value *= other.value; return *this; }
    DebugSizeT& operator/=(const DebugSizeT& other) { value /= other.value; return *this; }

    DebugSizeT& operator+=(const size_t other) { value += other; return *this; }
    DebugSizeT& operator-=(const size_t other) { value -= other; return *this; }
    DebugSizeT& operator*=(const size_t other) { value *= other; return *this; }
    DebugSizeT& operator/=(const size_t other) { value /= other; return *this; }

    // Comparisons
    bool operator==(const DebugSizeT& other) const { return value == other.value; }
    bool operator!=(const DebugSizeT& other) const { return value != other.value; }
    bool operator<(const DebugSizeT& other) const { return value < other.value; }
    bool operator>(const DebugSizeT& other) const { return value > other.value; }
    bool operator<=(const DebugSizeT& other) const { return value <= other.value; }
    bool operator>=(const DebugSizeT& other) const { return value >= other.value; }

    bool operator==(const size_t other) const { return value == other; }
    bool operator!=(const size_t other) const { return value != other; }
    bool operator<(const size_t other) const { return value < other; }
    bool operator>(const size_t other) const { return value > other; }
    bool operator<=(const size_t other) const { return value <= other; }
    bool operator>=(const size_t other) const { return value >= other; }

    bool operator==(const int other) const { return value == static_cast<size_t>(other); }
    bool operator!=(const int other) const { return value != static_cast<size_t>(other); }
    bool operator<(const int other) const { return value < static_cast<size_t>(other); }
    bool operator>(const int other) const { return value > static_cast<size_t>(other); }
    bool operator<=(const int other) const { return value <= static_cast<size_t>(other); }
    bool operator>=(const int other) const { return value >= static_cast<size_t>(other); }
};

#endif //CROCOS_DBGSTDDEF_H