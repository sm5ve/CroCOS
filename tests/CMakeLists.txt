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
#
# ─── Why there is a target for it (R-18) ───────────────────────────────────
#
# The referee pass re-raised the default as a coverage hole. It is not — the
# decision above is deliberate and the trade-off is the one already written. What
# WAS a hole is the distance between "run them deliberately" and doing so: the line
# above reconfigures the tree, which nobody does casually, so in practice the
# opt-in never happened. `run_tsan_stress` below closes that without touching the
# default: it configures a SEPARATE build directory with the flag off and runs the
# one runner the flag affects, leaving this build untouched.
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

# ─── The deliberate race-detector run (R-18) ───────────────────────────────
#
# `CROCOS_SKIP_TSAN_STRESS` defaults ON for the reason documented at the option,
# and this is how you take the other branch without reconfiguring your working
# build. A separate binary directory, so switching back is not a rebuild.
#
# Only `CoreTestRunnerTSan` is affected — the sixteen gated tests all live in
# `tests/core` (AtomicBitPool, SplitBitmap, TreiberStack, AtomicLinkedList and the
# `Core::rcu::EpochDomain` concurrent trio), and `tests/core/CMakeLists.txt` sets
# the define on that runner alone. Building the whole tree here would cost minutes
# and add no coverage.
#
# **Run it on a quiet machine.** The gated tests' timeouts are liveness checks, and
# under TSan on a loaded box they become performance assertions — which is the
# entire finding behind the default. A red result here with other work running says
# nothing.
add_custom_target(run_tsan_stress
    USES_TERMINAL
    COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}
                             -B ${CMAKE_CURRENT_BINARY_DIR}/tsan-stress
                             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                             -DCROCOS_SKIP_TSAN_STRESS=OFF
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}/tsan-stress
                             --target CoreTestRunnerTSan --parallel
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tsan-stress/core/CoreTestRunnerTSan
    COMMENT "Running the load-sensitive concurrency stress under TSan (quiet machine only)"
)