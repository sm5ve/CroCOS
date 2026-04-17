//
// CroCOS Unit Test Harness - Assertions and Test Runner
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_TESTHARNESS_H
#define CROCOS_TESTHARNESS_H

#include "../assert_support.h"
#include "MemoryTracker.h"
#include <source_location>
#include <string>

namespace CroCOSTest {
    
    // Forward declarations
    struct TestInfo;
    struct TestCleanupHook;
    
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
    private:
        // Helper methods for test discovery and execution
        static const TestInfo* const* getTests(size_t& testCount);
        static const TestCleanupHook* const* getCleanupHooks(size_t& hookCount);
        static TestResult runSingleTest(const TestInfo* test);

    public:
        static int runAllTests();
        static int runTest(const char* testName);
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

	#define ASSERT_UNREACHABLE(message) \
        throw CroCOSTest::AssertionFailure("Assertion failed: unreachable: " #message);
    
    // Binary comparison assertions are template functions rather than macros so that
    // arguments containing commas (e.g. multi-dimensional subscripts like arr[i, j, k])
    // are parsed correctly by the C++ parser instead of being split by the preprocessor.
    template <typename A, typename B>
    inline void ASSERT_EQ(const A& expected, const B& actual,
                          std::source_location loc = std::source_location::current()) {
        if (!(expected == actual))
            throw AssertionFailure("Assertion failed: expected == actual at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    template <typename A, typename B>
    inline void ASSERT_NE(const A& expected, const B& actual,
                          std::source_location loc = std::source_location::current()) {
        if (expected == actual)
            throw AssertionFailure("Assertion failed: expected != actual at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    template <typename A, typename B>
    inline void ASSERT_LT(const A& a, const B& b,
                          std::source_location loc = std::source_location::current()) {
        if (!(a < b))
            throw AssertionFailure("Assertion failed: a < b at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    template <typename A, typename B>
    inline void ASSERT_LE(const A& a, const B& b,
                          std::source_location loc = std::source_location::current()) {
        if (!(a <= b))
            throw AssertionFailure("Assertion failed: a <= b at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    template <typename A, typename B>
    inline void ASSERT_GT(const A& a, const B& b,
                          std::source_location loc = std::source_location::current()) {
        if (!(a > b))
            throw AssertionFailure("Assertion failed: a > b at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    template <typename A, typename B>
    inline void ASSERT_GE(const A& a, const B& b,
                          std::source_location loc = std::source_location::current()) {
        if (!(a >= b))
            throw AssertionFailure("Assertion failed: a >= b at " +
                                   std::string(loc.file_name()) + ":" + std::to_string(loc.line()));
    }

    #define ASSERT_NO_ALLOCS() \
        do { \
            if (MemoryTracker::getTotalAllocated() != 0) { \
                throw CroCOSTest::AssertionFailure("Assertion failed: test should not have allocated any memory"); \
            } \
        } while(0)

    // Test information structure stored in custom section
    struct TestInfo {
        const char* name;
        void (*testFunc)();
        const char* fileName;
        int lineNumber;
        int timeoutMs = 0;   // 0 means no timeout
        bool noTracking = false;  // true disables memory leak detection for this test
    };

    // Between-test cleanup hook: a zero-argument function registered to run
    // after every test completes (pass or fail).  Use REGISTER_TEST_CLEANUP
    // to register a function; the harness calls all registered hooks in
    // declaration order before moving to the next test.
    struct TestCleanupHook {
        void (*cleanupFunc)();
    };

        // Cross-platform section attributes
    #ifdef __APPLE__
    #define CROCOS_TEST_SECTION         __attribute__((used, section("__DATA,crocos_tests")))
    #define CROCOS_CLEANUP_SECTION      __attribute__((used, section("__DATA,crocos_cleanup")))
    #else
    #define CROCOS_TEST_SECTION         __attribute__((used, section(".crocos_unit_tests")))
    #define CROCOS_CLEANUP_SECTION      __attribute__((used, section(".crocos_test_cleanup")))
    #endif

        // Test registration macro using custom section (cross-platform)
    #define TEST(testName) \
    void testName(); \
    namespace { \
    const CroCOSTest::TestInfo testName##_info { \
    #testName, \
    testName, \
    __FILE__, \
    __LINE__, \
    0 \
    }; \
    CROCOS_TEST_SECTION \
    const CroCOSTest::TestInfo* testName##_registration = &testName##_info; \
    } \
    void testName()

        // Test registration macro with a timeout (in milliseconds)
    #define TEST_WITH_TIMEOUT(testName, timeoutMilliseconds) \
    void testName(); \
    namespace { \
    const CroCOSTest::TestInfo testName##_info { \
    #testName, \
    testName, \
    __FILE__, \
    __LINE__, \
    timeoutMilliseconds \
    }; \
    CROCOS_TEST_SECTION \
    const CroCOSTest::TestInfo* testName##_registration = &testName##_info; \
    } \
    void testName()

        // Test registration macro with a timeout and memory tracking disabled.
        // Use when the test is inherently multi-threaded and the code under test
        // makes no dynamic heap allocations (so leak detection adds no value).
    #define TEST_WITH_TIMEOUT_NO_TRACKING(testName, timeoutMilliseconds) \
    void testName(); \
    namespace { \
    const CroCOSTest::TestInfo testName##_info { \
    #testName, \
    testName, \
    __FILE__, \
    __LINE__, \
    timeoutMilliseconds, \
    true \
    }; \
    CROCOS_TEST_SECTION \
    const CroCOSTest::TestInfo* testName##_registration = &testName##_info; \
    } \
    void testName()

    // Register a cleanup function to be called after every test.
    // Usage: REGISTER_TEST_CLEANUP(myCleanupFn)  (no semicolon on the function itself)
    #define REGISTER_TEST_CLEANUP(cleanupFn) \
    namespace { \
    const CroCOSTest::TestCleanupHook cleanupFn##_hook { cleanupFn }; \
    CROCOS_CLEANUP_SECTION \
    const CroCOSTest::TestCleanupHook* cleanupFn##_hook_registration = &cleanupFn##_hook; \
    }
}

using namespace CroCOSTest;

#endif //CROCOS_TESTHARNESS_H