//
// CroCOS Unit Test Harness Implementation
// Created by Spencer Martin on 7/24/25.
//

#include "TestHarness.h"
#include "MemoryTracker.h"
#include <vector>

// Cross-platform section boundary access
#ifdef __APPLE__
    // On macOS, we need to use getsectiondata to access the custom section
    #include <mach-o/getsect.h>
    #include <mach-o/dyld.h>
    
    static const CroCOSTest::TestInfo** getMacOSTests(unsigned long* count) {
        // Get the main executable's mach header
        const struct mach_header_64* header = nullptr;
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            if (_dyld_get_image_name(i)[0] == '/' && 
                strstr(_dyld_get_image_name(i), "CoreLibraryTests")) {
                header = (const struct mach_header_64*)_dyld_get_image_header(i);
                break;
            }
        }
        
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

namespace CroCOSTest {
    
    int TestRunner::runAllTests() {
        std::vector<TestResult> results;
        
        std::cout << "Starting CroCOS Core Library Tests\n" << std::endl;
        
        // Get tests using cross-platform approach
        const CroCOSTest::TestInfo** tests;
        size_t testCount;
        
#ifdef __APPLE__
        unsigned long macCount;
        tests = getMacOSTests(&macCount);
        testCount = macCount;
#else
        if (__crocos_unit_tests_start == nullptr || __crocos_unit_tests_end == nullptr || 
            __crocos_unit_tests_start >= __crocos_unit_tests_end) {
            tests = nullptr;
            testCount = 0;
        } else {
            tests = __crocos_unit_tests_start;
            testCount = __crocos_unit_tests_end - __crocos_unit_tests_start;
        }
#endif

        if (!tests || testCount == 0) {
            std::cout << "No tests found!" << std::endl;
            return 0;
        }
        
        // Run all tests using unified approach
        for (size_t i = 0; i < testCount; ++i) {
            const CroCOSTest::TestInfo* test = tests[i];
            if (test == nullptr) continue;  // Skip null entries
            
            std::cout << "Running test: " << test->name << "..." << std::endl;
            
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
                    results.emplace_back(test->name, false, leakMsg);
                    std::cout << "  ✗ FAILED: " << leakMsg << std::endl;
                    
                    // Print detailed leak report for this test
                    std::cout << "  Memory leak details for " << test->name << ":" << std::endl;
                    MemoryTracker::printLeakReport();
                } else {
                    results.emplace_back(test->name, true);
                    std::cout << "  ✓ PASSED (Memory: " << MemoryTracker::getTotalAllocated() 
                              << " bytes allocated, " << MemoryTracker::getTotalFreed() 
                              << " bytes freed)" << std::endl;
                }
            } catch (const AssertionFailure& e) {
                results.emplace_back(test->name, false, e.what());
                std::cout << "  ✗ FAILED: " << e.what() << std::endl;
            } catch (const std::exception& e) {
                results.emplace_back(test->name, false, e.what());
                std::cout << "  ✗ FAILED: " << e.what() << std::endl;
            } catch (...) {
                results.emplace_back(test->name, false, "Unknown exception");
                std::cout << "  ✗ FAILED: Unknown exception" << std::endl;
            }
        }
        
        // Print summary
        int passed = 0, failed = 0;
        std::cout << "\n=== Test Summary ===" << std::endl;
        for (const auto& result : results) {
            if (result.passed) {
                passed++;
            } else {
                failed++;
                std::cout << "FAILED: " << result.testName;
                if (result.errorMessage.length() > 0) {
                    std::cout << " - " << result.errorMessage;
                }
                std::cout << std::endl;
            }
        }
        
        std::cout << "\nTotal: " << results.size() 
                  << ", Passed: " << passed 
                  << ", Failed: " << failed << std::endl;
        
        return failed > 0 ? 1 : 0;
    }
}