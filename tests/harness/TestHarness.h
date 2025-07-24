//
// CroCOS Unit Test Harness - Assertions and Test Runner
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_TESTHARNESS_H
#define CROCOS_TESTHARNESS_H

#include <iostream>
#include <string>
#include <sstream>
#include "../TestFramework.h"

namespace TestHarness {
    
    // Test assertion failure exception
    class AssertionFailure : public std::exception {
    private:
        std::string message;
    public:
        explicit AssertionFailure(const std::string& msg) : message(msg) {}
        const char* what() const noexcept override { return message.c_str(); }
    };
    
    // Test result tracking
    struct TestResult {
        const char* testName;
        bool passed;
        const char* errorMessage;
        
        TestResult(const char* name, bool success, const char* error = nullptr)
            : testName(name), passed(success), errorMessage(error) {}
    };
    
    // Test runner class
    class TestRunner {
    public:
        static int runAllTests();
    };
    
    // Assertion macros
    #define ASSERT_TRUE(condition) \
        do { \
            if (!(condition)) { \
                throw TestHarness::AssertionFailure("Assertion failed: " #condition); \
            } \
        } while(0)
    
    #define ASSERT_FALSE(condition) \
        do { \
            if (condition) { \
                throw TestHarness::AssertionFailure("Assertion failed: expected false but got true: " #condition); \
            } \
        } while(0)
    
    #define ASSERT_EQ(expected, actual) \
        do { \
            if (!((expected) == (actual))) { \
                throw TestHarness::AssertionFailure("Assertion failed: expected == actual (" #expected " == " #actual ")"); \
            } \
        } while(0)
    
    #define ASSERT_NE(expected, actual) \
        do { \
            if ((expected) == (actual)) { \
                throw TestHarness::AssertionFailure("Assertion failed: expected != actual (" #expected " != " #actual ")"); \
            } \
        } while(0)
    
    #define ASSERT_LT(a, b) \
        do { \
            if (!((a) < (b))) { \
                throw TestHarness::AssertionFailure("Assertion failed: " #a " < " #b); \
            } \
        } while(0)
    
    #define ASSERT_LE(a, b) \
        do { \
            if (!((a) <= (b))) { \
                throw TestHarness::AssertionFailure("Assertion failed: " #a " <= " #b); \
            } \
        } while(0)
    
    #define ASSERT_GT(a, b) \
        do { \
            if (!((a) > (b))) { \
                throw TestHarness::AssertionFailure("Assertion failed: " #a " > " #b); \
            } \
        } while(0)
    
    #define ASSERT_GE(a, b) \
        do { \
            if (!((a) >= (b))) { \
                throw TestHarness::AssertionFailure("Assertion failed: " #a " >= " #b); \
            } \
        } while(0)
    
    // Helper function to format assert messages with variadic args
    template<typename... Args>
    std::string formatAssertMessage(const Args&... args) {
        std::ostringstream oss;
        ((oss << args), ...);
        return oss.str();
    }
}

#endif //CROCOS_TESTHARNESS_H