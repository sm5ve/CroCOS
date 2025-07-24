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
    core/VectorTest.cpp
    core/HeapTest.cpp
    core/GraphTest.cpp
)

# Add Core library sources directly to the test executable
target_sources(CoreLibraryTests PRIVATE
    ../libraries/Core/PrintStream.cpp
    ../libraries/Core/str.cpp
    ../libraries/Core/atomic/atomic.cpp
)

target_link_libraries(CoreLibraryTests PRIVATE
    TestHarness
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
    # Force include test.h to inject strong allocation symbols for objcopy
    -include ${CMAKE_CURRENT_SOURCE_DIR}/test.h
)

# Custom PRE_LINK command to instrument object files before linking
# Major shenanigans to get around the fact that the macOS linker doesn't support --wrap
add_custom_command(TARGET CoreLibraryTests PRE_LINK
    COMMENT "Instrumenting object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR}
)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(CoreLibraryTests PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Memory tracking is enabled by overriding allocation functions in MemoryTracker.cpp
# No special linker options needed - our definitions will override system ones

# Add a custom target to run tests
add_custom_target(run_tests
    COMMAND CoreLibraryTests
    DEPENDS CoreLibraryTests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS Core Library unit tests"
)