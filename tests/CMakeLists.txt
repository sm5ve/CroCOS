# CroCOS Core Library Unit Tests
# Created by Spencer Martin on 7/24/25.
# 
# This CMakeLists.txt is designed to be used independently from the main build
# to test the Core library using the host compiler while maintaining freestanding mode

cmake_minimum_required(VERSION 3.20)

# Use Homebrew clang with C++26 support, fallback to system clang
find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    set(CMAKE_C_COMPILER ${HOMEBREW_CLANG})
    set(CMAKE_CXX_COMPILER ${HOMEBREW_CLANGXX})
    message(STATUS "Using Homebrew clang: ${HOMEBREW_CLANG}")
else()
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    message(STATUS "Using system clang (fallback)")
endif()

project(CroCOSTests LANGUAGES C CXX)

# Don't use CMAKE_CXX_STANDARD since it might not recognize C++26
# We'll use explicit flags instead

# We'll instrument both Core library and test files together in the main executable

# Create the test harness library (uses standard library)
add_library(TestHarness STATIC)

target_sources(TestHarness PRIVATE
    harness/TestHarness.cpp
    harness/MemoryTracker.cpp
)

target_include_directories(TestHarness PUBLIC
    .
    harness
)

# Test harness can use standard library and exceptions
target_compile_options(TestHarness PRIVATE
    -std=c++2c
    -Wall
    -Wextra
    -g
)

# Link against C++ standard library for std::unordered_map, etc.
if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
target_link_libraries(TestHarness PUBLIC
    /opt/homebrew/opt/llvm/lib/c++/libc++.a
    /opt/homebrew/opt/llvm/lib/c++/libc++abi.a
)
else()
target_link_libraries(TestHarness PUBLIC
    c++
)
endif()

# ─── CROCOS_SKIP_TSAN_STRESS ───────────────────────────────────────────────
#
# Sixteen timing-bounded concurrency stress tests (AtomicBitPool, SplitBitmap,
# TreiberStack, AtomicLinkedList, the RCU engine's concurrent trio) carry
# wall-clock timeouts. Those timeouts are LIVENESS checks — "did this hang?" —
# and under TSan's ~10-20x slowdown they become performance assertions.
#
# Diagnosed rather than guessed: on an idle machine CoreTestRunnerTSan is
# 441/441 across repeated runs; with two other TSan runners in parallel it is
# 425/441, failing exactly those sixteen, reproducibly in both directions. It is
# starvation, not flakiness.
#
# **Default ON** (Spencer's call, 2026-08-08). The diagnosis does not make the
# failures cheap: a red gate that is usually a lie costs real time to triage
# every time it appears, and a test that misleads more often than it informs is
# worse than one that is absent and documented. What turning it off buys is that
# these are the only tests exercising the lock-free structures under a race
# detector — so run them deliberately, on a quiet machine, rather than
# incidentally:
#
#     cmake -S tests -B tests/build -DCROCOS_SKIP_TSAN_STRESS=OFF
#
# The ASan and uninstrumented builds run them unconditionally either way, so the
# tests themselves never stop being exercised — only their race-detector
# coverage is opt-in.
option(CROCOS_SKIP_TSAN_STRESS
       "Skip the load-sensitive concurrency stress tests in TSan builds" ON)

add_subdirectory(kernel)
add_subdirectory(liballoc)
add_subdirectory(core)

# Add a custom target to run all tests (core + kernel)
# Every ASan runner is paired with its TSan sibling, so `run_all_tests` genuinely
# means all tests. The two Core/LibAlloc TSan runners were omitted for a long
# time; CoreTestRunnerTSan in particular is the ONLY thing covering
# Core::rcu::EpochDomain's concurrency tests under the ARMv8 release gate, so its
# absence meant the RCU engine's weak-memory coverage was opt-in.


add_custom_target(run_all_tests
    USES_TERMINAL
    COMMAND core/CoreTestRunner
    COMMAND core/CoreTestRunnerTSan
    COMMAND kernel/KernelTestRunner
    COMMAND liballoc/LibAllocTestRunner
    COMMAND liballoc/LibAllocTestRunnerTSan
    COMMAND kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunner
    COMMAND kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunnerTSan
    COMMAND kernel/rcu/KernelRcuIntegrationTestRunner
    COMMAND kernel/rcu/KernelRcuIntegrationTestRunnerTSan
    # The radix runners were missing here for the whole of Phases 0-2, which is
    # the same omission the comment above records for the Core/LibAlloc TSan
    # siblings: a phase verified by running its own runner does not notice a
    # sibling it broke (D-021), and `run_all_tests` not covering the subsystem
    # under active development is exactly the wrong way round. The progress-audit
    # runner stays out — it is a deliberately expensive instrument, not a gate.
    COMMAND kernel/radix/KernelRadixTestRunner
    COMMAND kernel/radix/KernelRadixTestRunnerTSan
    DEPENDS core/CoreTestRunner core/CoreTestRunnerTSan
            liballoc/LibAllocTestRunner liballoc/LibAllocTestRunnerTSan
            kernel/KernelTestRunner
            kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunner
            kernel/vmsmalloc/KernelVmsmallocIntegrationTestRunnerTSan
            kernel/rcu/KernelRcuIntegrationTestRunner
            kernel/rcu/KernelRcuIntegrationTestRunnerTSan
            kernel/radix/KernelRadixTestRunner
            kernel/radix/KernelRadixTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all CroCOS unit tests"
)