//
// CroCOS Unit Test Harness - Assertions and Test Runner
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_TESTHARNESS_H
#define CROCOS_TESTHARNESS_H

#include <iostream>
#include <string>
#include <sstream>
#include "TestHarness.h"

namespace CroCOSTest {
    
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
        std::string errorMessage;
        
        TestResult(const char* name, bool success, std::string error = "")
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
                throw CroCOSTest::AssertionFailure("Assertion failed: " #condition); \
            } \
        } while(0)
    
    #define ASSERT_FALSE(condition) \
        do { \
            if (condition) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: expected false but got true: " #condition); \
            } \
        } while(0)
    
    #define ASSERT_EQ(expected, actual) \
        do { \
            if (!((expected) == (actual))) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: expected == actual (" #expected " == " #actual ")"); \
            } \
        } while(0)
    
    #define ASSERT_NE(expected, actual) \
        do { \
            if ((expected) == (actual)) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: expected != actual (" #expected " != " #actual ")"); \
            } \
        } while(0)
    
    #define ASSERT_LT(a, b) \
        do { \
            if (!((a) < (b))) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: " #a " < " #b); \
            } \
        } while(0)
    
    #define ASSERT_LE(a, b) \
        do { \
            if (!((a) <= (b))) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: " #a " <= " #b); \
            } \
        } while(0)
    
    #define ASSERT_GT(a, b) \
        do { \
            if (!((a) > (b))) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: " #a " > " #b); \
            } \
        } while(0)
    
    #define ASSERT_GE(a, b) \
        do { \
            if (!((a) >= (b))) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: " #a " >= " #b); \
            } \
        } while(0)
    
    // Helper function to format assert messages with variadic args
    template<typename... Args>
    std::string formatAssertMessage(const Args&... args) {
        std::ostringstream oss;
        ((oss << args), ...);
        return oss.str();
    }

    // Test information structure stored in custom section
    struct TestInfo {
        const char* name;
        void (*testFunc)();
        const char* fileName;
        int lineNumber;
    };

        // Cross-platform section attribute
    #ifdef __APPLE__
    #define CROCOS_TEST_SECTION __attribute__((used, section("__DATA,crocos_tests")))
    #else
    #define CROCOS_TEST_SECTION __attribute__((used, section(".crocos_unit_tests")))
    #endif

        // Test registration macro using custom section (cross-platform)
    #define TEST(testName) \
    void testName(); \
    namespace { \
    const CroCOSTest::TestInfo testName##_info { \
    #testName, \
    testName, \
    __FILE__, \
    __LINE__ \
    }; \
    CROCOS_TEST_SECTION \
    const CroCOSTest::TestInfo* testName##_registration = &testName##_info; \
    } \
    void testName()
}

#endif //CROCOS_TESTHARNESS_H