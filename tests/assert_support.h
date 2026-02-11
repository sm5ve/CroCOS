//
// Created by Spencer Martin on 7/27/25.
//

#ifndef ASSERT_SUPPORT_H
#define ASSERT_SUPPORT_H

#include <iostream>
#include <string>
#include <sstream>
#include <type_traits>

// Forward declarations for memory tracking control
namespace CroCOSTest {
    void pauseTracking();
    void resumeTracking();
    bool getTrackingStatus();
}

namespace CroCOSTest{
    // Test assertion failure exception
    class AssertionFailure : public std::exception {
    private:
        std::string message;
    public:
        explicit AssertionFailure(const std::string& msg) : message(msg) {}
        const char* what() const noexcept override { return message.c_str(); }
    };

    // Helper to convert argument to string (simple version that avoids ostringstream/locale)
    template<typename T>
    std::string toString(const T& value) {
        using DecayedT = typename std::decay<T>::type;
        // For C-strings and string literals
        if constexpr (std::is_same<DecayedT, const char*>::value ||
                      std::is_same<DecayedT, char*>::value) {
            return std::string(value);
        }
        // For std::string
        else if constexpr (std::is_same<DecayedT, std::string>::value) {
            return value;
        }
        // For other types, fall back to std::to_string or ostringstream
        else {
            // Pause tracking to avoid locale issues with ostringstream
            pauseTracking();
            std::ostringstream oss;
            oss << value;
            std::string result = oss.str();
            resumeTracking();
            return result;
        }
    }

    // Helper function to format assert messages with variadic args
    // Note: We avoid using std::ostringstream directly on the full message to minimize
    // locale-related issues with AddressSanitizer
    template<typename... Args>
    std::string formatAssertMessage(const Args&... args) {
        std::string result;
        ((result += toString(args)), ...);
        return result;
    }
}

#endif //ASSERT_SUPPORT_H
