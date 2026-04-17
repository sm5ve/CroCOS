// StressAssertSupport.h
// Force-included into kernel/core sources to satisfy the CroCOSTest namespace
// that assert.h (under CORE_LIBRARY_TESTING) requires.
// In the stress harness, an assertion failure prints to stderr and aborts.

#pragma once
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sstream>
#include <type_traits>
#include <exception>

namespace CroCOSTest {
    inline void pauseTracking()          {}
    inline void resumeTracking()         {}
    inline bool getTrackingStatus()      { return false; }

    struct AssertionFailure final : std::exception {
        std::string message;
        explicit AssertionFailure(std::string msg) : message(std::move(msg)) {
            fprintf(stderr, "\n[STRESS] ASSERT FAILED: %s\n", message.c_str());
            fflush(stderr);
            abort();
        }
        const char* what() const noexcept override { return message.c_str(); }
    };

    template<typename T>
    std::string toString(const T& value) {
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>)
            return std::string(value);
        else if constexpr (std::is_same_v<D, std::string>)
            return value;
        else {
            std::ostringstream oss;
            oss << value;
            return oss.str();
        }
    }

    template<typename... Args>
    std::string formatAssertMessage(const Args&... args) {
        std::string result;
        ((result += toString(args)), ...);
        return result;
    }
}
