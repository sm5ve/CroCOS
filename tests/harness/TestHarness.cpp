//
// CroCOS Unit Test Harness Implementation
// Created by Spencer Martin on 7/24/25.
//

#include "TestHarness.h"
#include "MemoryTracker.h"
#include <vector>
#include <cstdio>
#include "assert.h"

// Cross-platform section boundary access
#ifdef __APPLE__
    // On macOS, we need to use getsectiondata to access the custom section
    #include <mach-o/getsect.h>
    #include <mach-o/dyld.h>
    
    static const CroCOSTest::TestInfo** getMacOSTests(unsigned long* count) {
        // Get the main executable's mach header (index 0 is always the main executable)
        const struct mach_header_64* header = (const struct mach_header_64*)_dyld_get_image_header(0);

        if (!header) {
            *count = 0;
            return nullptr;
        }
        
        unsigned long size;
        const CroCOSTest::TestInfo** section_data = 
            (const CroCOSTest::TestInfo**)getsectiondata(header, "__DATA", "crocos_tests", &size);

        if (section_data) {
            *count = size / sizeof(const CroCOSTest::TestInfo*);
            return section_data;
        }
        
        *count = 0;
        return nullptr;
    }
#else
    // Linux/ELF format using weak symbols
    extern "C" const CroCOSTest::TestInfo* __crocos_unit_tests_start[] __attribute__((weak));
    extern "C" const CroCOSTest::TestInfo* __crocos_unit_tests_end[] __attribute__((weak));
#endif

// Function pointer approach - more reliable across platforms
extern "C" void presort_object_parent_lists(void) __attribute__((weak));

// Safe wrapper to check if the function exists
static void call_presort_if_exists() {
    // On some platforms, we need to check the function pointer directly
    void (*presort_ptr)(void) = &presort_object_parent_lists;
    if (presort_ptr && presort_ptr != (void*)0) {
        presort_object_parent_lists();
    }
}

namespace CroCOSTest {
    
    const TestInfo* const* TestRunner::getTests(size_t& testCount) {
#ifdef __APPLE__
        unsigned long macCount;
        const TestInfo** tests = getMacOSTests(&macCount);
        testCount = macCount;
        return tests;
#else
        if (__crocos_unit_tests_start == nullptr || __crocos_unit_tests_end == nullptr || 
            __crocos_unit_tests_start >= __crocos_unit_tests_end) {
            testCount = 0;
            return nullptr;
        } else {
            testCount = __crocos_unit_tests_end - __crocos_unit_tests_start;
            return __crocos_unit_tests_start;
        }
#endif
    }
    
    TestResult TestRunner::runSingleTest(const TestInfo* test) {
        printf("Running test: %s...\n", test->name);
        fflush(stdout);
        
        // Reset memory tracking before each test
        MemoryTracker::reset();
        
        try {
            test->testFunc();
            
            // Check for memory leaks after test completion
            if (MemoryTracker::hasLeaks()) {
                std::string leakMsg = "Memory leak detected: " + 
                                    std::to_string(MemoryTracker::getCurrentUsage()) + 
                                    " bytes leaked in " + 
                                    std::to_string(MemoryTracker::getActiveAllocationCount()) + 
                                    " allocations";
                std::cout << "  ✗ FAILED: " << leakMsg << std::endl;
                
                // Print detailed leak report for this test
                std::cout << "  Memory leak details for " << test->name << ":" << std::endl;
                MemoryTracker::printLeakReport();
                return TestResult(test->name, false, leakMsg);
            } else {
                // Use printf to avoid std::cout locale issues
                size_t allocated = MemoryTracker::getTotalAllocated();
                size_t freed = MemoryTracker::getTotalFreed();
                printf("  ✓ PASSED (Memory: %zu bytes allocated, %zu bytes freed)\n", allocated, freed);
                fflush(stdout);
                return TestResult(test->name, true);
            }
        } catch (const AssertionFailure& e) {
            std::cout << "  ✗ FAILED: " << e.what() << std::endl;
            return TestResult(test->name, false, e.what());
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAILED: " << e.what() << std::endl;
            return TestResult(test->name, false, e.what());
        } catch (...) {
            std::cout << "  ✗ FAILED: Unknown exception" << std::endl;
            return TestResult(test->name, false, "Unknown exception");
        }
    }
    
    int TestRunner::runAllTests() {
        call_presort_if_exists();

        std::vector<TestResult> results;

        //TODO update this to be configurable
        std::cout << "Starting CroCOS Core Library Tests\n" << std::endl;
        
        // Get tests using helper method
        size_t testCount;
        const TestInfo* const* tests = getTests(testCount);

        if (!tests || testCount == 0) {
            std::cout << "No tests found!" << std::endl;
            return 0;
        }

        // Count actual non-null tests (AddressSanitizer creates padding entries)
        size_t actualTestCount = 0;
        for (size_t i = 0; i < testCount; ++i) {
            if (tests[i] != nullptr) actualTestCount++;
        }

        printf("Found %zu tests\n\n", actualTestCount);
        fflush(stdout);

        // Run all tests
        for (size_t i = 0; i < testCount; ++i) {
            const TestInfo* test = tests[i];
            if (test == nullptr) continue;  // Skip null entries

            results.push_back(runSingleTest(test));
        }
        
        // Print summary
        int passed = 0, failed = 0;
        printf("\n=== Test Summary ===\n");
        fflush(stdout);
        for (const auto& result : results) {
            if (result.passed) {
                passed++;
            } else {
                failed++;
                printf("FAILED: %s", result.testName);
                if (result.errorMessage.length() > 0) {
                    printf(" - %s", result.errorMessage.c_str());
                }
                printf("\n");
                fflush(stdout);
            }
        }

        printf("\nTotal: %zu, Passed: %d, Failed: %d\n", results.size(), passed, failed);
        fflush(stdout);
        
        return failed > 0 ? 1 : 0;
    }
    
    int TestRunner::runTest(const char* testName) {
        call_presort_if_exists();
        
        // Get tests using helper method
        size_t testCount;
        const TestInfo* const* tests = getTests(testCount);

        if (!tests || testCount == 0) {
            std::cout << "No tests found!" << std::endl;
            return 1;
        }
        
        // Find and run the specific test
        for (size_t i = 0; i < testCount; ++i) {
            const TestInfo* test = tests[i];
            if (test == nullptr) continue;  // Skip null entries
            
            // Check if this is the test we're looking for
            if (strcmp(test->name, testName) == 0) {
                TestResult result = runSingleTest(test);
                return result.passed ? 0 : 1;
            }
        }
        
        std::cout << "Test '" << testName << "' not found!" << std::endl;
        std::cout << "Available tests:" << std::endl;
        for (size_t i = 0; i < testCount; ++i) {
            const TestInfo* test = tests[i];
            if (test != nullptr) {
                std::cout << "  " << test->name << std::endl;
            }
        }
        return 1;
    }
}