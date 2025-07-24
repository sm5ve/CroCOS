# CroCOS Core Library Unit Tests
# Created by Spencer Martin on 7/24/25.
# 
# This CMakeLists.txt is designed to be used independently from the main build
# to test the Core library using the host compiler while maintaining freestanding mode

cmake_minimum_required(VERSION 3.20)

# Try using clang with explicit C++26 support
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(CroCOSTests LANGUAGES C CXX)

# Don't use CMAKE_CXX_STANDARD since it might not recognize C++26
# We'll use explicit flags instead

# Create a test-specific version of the Core library compiled in freestanding mode
add_library(CoreTest STATIC)

# Add Core library sources (same as CoreKernel but with host compiler)
# Skip Object.cpp for now due to section attribute issues on macOS
target_sources(CoreTest PRIVATE
    ../libraries/Core/PrintStream.cpp
    ../libraries/Core/str.cpp
    ../libraries/Core/atomic/atomic.cpp
)

# Configure CoreTest with freestanding compilation but host compiler
target_include_directories(CoreTest PUBLIC 
    ../libraries/Core/include
    ../kernel/include
)

# Use freestanding mode to match kernel compilation environment
target_compile_options(CoreTest PRIVATE
    -ffreestanding
    -nostdlib++
    -fno-exceptions  # We'll enable exceptions only for test harness
    -fno-rtti
    -std=c++26  # Use c++26 instead of gnu++26 for clang
    -Wall
    -Wextra
    -Wno-writable-strings  # Allow string literal conversions for testing
    -Wno-ignored-attributes  # Allow used attributes that might be ignored
    -Wno-c++23-extensions  # Allow C++23 extensions
    -Wno-invalid-constexpr  # Allow invalid constexpr for testing
    -DCORE_LIBRARY_TESTING  # Define to help resolve conflicts
    -Werror
    # Remove x86-64 specific flags for host compilation
)

target_compile_definitions(CoreTest PRIVATE
    CORE_LINKED_WITH_TESTS=1
)

# Create the test harness library (uses standard library)
add_library(TestHarness STATIC)

target_sources(TestHarness PRIVATE
    harness/TestHarness.cpp
)

target_include_directories(TestHarness PUBLIC
    .
    harness
)

# Test harness can use standard library and exceptions
target_compile_options(TestHarness PRIVATE
    -std=c++20
    -Wall
    -Wextra
)

# Create test executable
add_executable(CoreLibraryTests)

target_sources(CoreLibraryTests PRIVATE
    TestMain.cpp
)

# Add test source files
target_sources(CoreLibraryTests PRIVATE 
    core/BasicTest.cpp
    core/VectorTest.cpp
    core/HeapTest.cpp
)

target_link_libraries(CoreLibraryTests PRIVATE
    TestHarness
    CoreTest
)

target_include_directories(CoreLibraryTests PRIVATE
    .
    ../libraries/Core/include
)

# Enable exceptions for the test executable (needed for test framework)
target_compile_options(CoreLibraryTests PRIVATE
    -std=c++20
    -fexceptions
    -Wall
    -Wextra
    -Wno-c++23-extensions  # Allow C++23 extensions used in Core library
    -DCORE_LIBRARY_TESTING  # Define to help resolve conflicts
)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(CoreLibraryTests PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Add a custom target to run tests
add_custom_target(run_tests
    COMMAND CoreLibraryTests
    DEPENDS CoreLibraryTests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS Core Library unit tests"
)