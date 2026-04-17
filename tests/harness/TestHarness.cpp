//
// CroCOS Unit Test Harness Implementation
// Created by Spencer Martin on 7/24/25.
//

#include "TestHarness.h"
#include "MemoryTracker.h"
#include <vector>
#include <cstdio>
#include <thread>
#include <future>
#include <chrono>
#include <memory>
#include "assert.h"

// Cross-platform section boundary access
#ifdef __APPLE__
    // On macOS, we need to use getsectiondata to access the custom section
    #include <mach-o/getsect.h>
    #include <mach-o/dyld.h>

    static const CroCOSTest::TestInfo** getMacOSTests(unsigned long* count) {
        const struct mach_header_64* header = (const struct mach_header_64*)_dyld_get_image_header(0);
        if (!header) { *count = 0; return nullptr; }
        unsigned long size;
        const CroCOSTest::TestInfo** section_data =
            (const CroCOSTest::TestInfo**)getsectiondata(header, "__DATA", "crocos_tests", &size);
        if (section_data) { *count = size / sizeof(const CroCOSTest::TestInfo*); return section_data; }
        *count = 0;
        return nullptr;
    }

    static const CroCOSTest::TestCleanupHook** getMacOSCleanupHooks(unsigned long* count) {
        const struct mach_header_64* header = (const struct mach_header_64*)_dyld_get_image_header(0);
        if (!header) { *count = 0; return nullptr; }
        unsigned long size;
        const CroCOSTest::TestCleanupHook** section_data =
            (const CroCOSTest::TestCleanupHook**)getsectiondata(header, "__DATA", "crocos_cleanup", &size);
        if (section_data) { *count = size / sizeof(const CroCOSTest::TestCleanupHook*); return section_data; }
        *count = 0;
        return nullptr;
    }
#else
    // Linux/ELF format using weak symbols
    extern "C" const CroCOSTest::TestInfo* __crocos_unit_tests_start[] __attribute__((weak));
    extern "C" const CroCOSTest::TestInfo* __crocos_unit_tests_end[] __attribute__((weak));
    extern "C" const CroCOSTest::TestCleanupHook* __crocos_test_cleanup_start[] __attribute__((weak));
    extern "C" const CroCOSTest::TestCleanupHook* __crocos_test_cleanup_end[] __attribute__((weak));
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

    const TestCleanupHook* const* TestRunner::getCleanupHooks(size_t& hookCount) {
#ifdef __APPLE__
        unsigned long macCount;
        const TestCleanupHook** hooks = getMacOSCleanupHooks(&macCount);
        hookCount = macCount;
        return hooks;
#else
        if (__crocos_test_cleanup_start == nullptr || __crocos_test_cleanup_end == nullptr ||
            __crocos_test_cleanup_start >= __crocos_test_cleanup_end) {
            hookCount = 0;
            return nullptr;
        } else {
            hookCount = __crocos_test_cleanup_end - __crocos_test_cleanup_start;
            return __crocos_test_cleanup_start;
        }
#endif
    }
    
    TestResult TestRunner::runSingleTest(const TestInfo* test) {
        printf("Running test: %s...\n", test->name);
        fflush(stdout);

        // Reset memory tracking before each test
        MemoryTracker::reset();

        auto handleException = [test](const std::string& message) -> TestResult {
            std::cout << "  ✗ FAILED: " << message << std::endl;
            return TestResult(test->name, false, message);
        };

        if (test->timeoutMs > 0) {
            // Run with timeout: execute the test on a background thread and wait
            // up to timeoutMs milliseconds for it to complete.
            //
            // The promise is heap-allocated via shared_ptr so that it stays alive
            // even after runSingleTest returns in the timeout path (the detached
            // thread holds the other reference and will eventually destroy it).
            auto promisePtr = std::make_shared<std::promise<void>>();
            auto future = promisePtr->get_future();

            std::thread testThread([promisePtr, test]() {
                try {
                    test->testFunc();
                    try { promisePtr->set_value(); } catch (...) {}
                } catch (const ThreadTerminationRequest&) {
                    // The MemoryTracker threw this to unwind our stack after a timeout.
                    // Exit cleanly without touching the promise — the main thread has
                    // already recorded a timeout failure and moved on.
                } catch (...) {
                    try { promisePtr->set_exception(std::current_exception()); } catch (...) {}
                }
            });

            if (future.wait_for(std::chrono::milliseconds(test->timeoutMs)) == std::future_status::timeout) {
                // Register the thread so the next allocation it makes throws
                // ThreadTerminationRequest, unwinding its stack gracefully.
                MemoryTracker::ignoreThread(testThread.get_id());
                // Clear any tracking state the timed-out thread may have dirtied.
                MemoryTracker::reset();
                testThread.detach();
                std::string msg = "Test timed out after " + std::to_string(test->timeoutMs) + "ms";
                printf("  ✗ FAILED: %s\n", msg.c_str());
                fflush(stdout);
                return TestResult(test->name, false, msg);
            }

            testThread.join();

            try {
                future.get();
            } catch (const AssertionFailure& e) {
                return handleException(e.what());
            } catch (const std::exception& e) {
                return handleException(e.what());
            } catch (...) {
                return handleException("Unknown exception");
            }
        } else {
            try {
                test->testFunc();
            } catch (const AssertionFailure& e) {
                return handleException(e.what());
            } catch (const std::exception& e) {
                return handleException(e.what());
            } catch (...) {
                return handleException("Unknown exception");
            }
        }

        // Run between-test cleanup hooks (registered via REGISTER_TEST_CLEANUP).
        // Hooks run after every test regardless of pass/fail, with tracking paused
        // so their internal bookkeeping does not appear as leaks.
        {
            size_t hookCount;
            const TestCleanupHook* const* hooks = getCleanupHooks(hookCount);
            if (hooks) {
                pauseTracking();
                for (size_t i = 0; i < hookCount; ++i) {
                    if (hooks[i] != nullptr)
                        hooks[i]->cleanupFunc();
                }
                resumeTracking();
            }
        }

        // Check for memory leaks after test completion (skip for noTracking tests)
        if (!test->noTracking && MemoryTracker::hasLeaks()) {
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
        }

        size_t allocated = MemoryTracker::getTotalAllocated();
        size_t freed = MemoryTracker::getTotalFreed();
        printf("  ✓ PASSED (Memory: %zu bytes allocated, %zu bytes freed)\n", allocated, freed);
        fflush(stdout);
        return TestResult(test->name, true);
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