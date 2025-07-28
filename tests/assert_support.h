//
// Created by Spencer Martin on 7/27/25.
//

#ifndef ASSERT_SUPPORT_H
#define ASSERT_SUPPORT_H

#include <iostream>
#include <string>
#include <sstream>

namespace CroCOSTest{
    // Test assertion failure exception
    class AssertionFailure : public std::exception {
    private:
        std::string message;
    public:
        explicit AssertionFailure(const std::string& msg) : message(msg) {}
        const char* what() const noexcept override { return message.c_str(); }
    };

    // Helper function to format assert messages with variadic args
    template<typename... Args>
    std::string formatAssertMessage(const Args&... args) {
        std::ostringstream oss;
        ((oss << args), ...);
        return oss.str();
    }
}

#endif //ASSERT_SUPPORT_H
