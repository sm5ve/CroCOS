//
// CroCOS Unit Test Framework - Test Registration
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_TESTFRAMEWORK_H
#define CROCOS_TESTFRAMEWORK_H

namespace CroCOSTest {
    
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

#endif //CROCOS_TESTFRAMEWORK_H