//
// CroCOS Unit Test Main Entry Point
// Created by Spencer Martin on 7/24/25.
//

#include "harness/TestHarness.h"

int main(int argc, char* argv[]) {
    if (argc > 1) {
        // Run specific test
        return CroCOSTest::TestRunner::runTest(argv[1]);
    } else {
        // Run all tests
        return CroCOSTest::TestRunner::runAllTests();
    }
}