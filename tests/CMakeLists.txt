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

# Create the core library test library
add_library(CoreLibraryTests STATIC)

# Add test source files
target_sources(CoreLibraryTests PRIVATE
    core/VectorTest.cpp
    core/HeapTest.cpp
    core/GraphTest.cpp
    core/ObjectTest.cpp
    core/SmartPointerTest.cpp
    core/OptionalTest.cpp
    core/VariantTest.cpp
)

# Add Core library sources directly to the test library
target_sources(CoreLibraryTests PRIVATE
    ../libraries/Core/PrintStream.cpp
    ../libraries/Core/str.cpp
    ../libraries/Core/atomic/atomic.cpp
    ../libraries/Core/Object.cpp
)

target_include_directories(CoreLibraryTests PUBLIC
    .
    ../libraries/Core/include
)

# Core library tests use freestanding mode but allow exceptions for test framework
target_compile_options(CoreLibraryTests PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -Wno-writable-strings
    -Wno-c++26-extensions  # Allow C++23 extensions used in Core library
    -DCORE_LIBRARY_TESTING  # Define to help resolve conflicts
    -DCROCOS_TESTING
    # Force include test.h to inject strong allocation symbols for objcopy
    -include ${CMAKE_CURRENT_SOURCE_DIR}/test.h
    -fsanitize=address
    -fsanitize=leak
    -g
)

# Custom PRE_LINK command to instrument object files before linking
# Major shenanigans to get around the fact that the macOS linker doesn't support --wrap
add_custom_command(TARGET CoreLibraryTests PRE_LINK
        COMMENT "Instrumenting object files for memory tracking before linking"
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} CoreLibraryTests
)

# Create test executable
add_executable(CoreLibraryTestRunner)

target_sources(CoreLibraryTestRunner PRIVATE
    TestMain.cpp
)

target_link_libraries(CoreLibraryTestRunner PRIVATE
    CoreLibraryTests
    TestHarness
)

target_include_directories(CoreLibraryTestRunner PRIVATE
    .
    ../libraries/Core/include
)

# Test runner needs exceptions and standard library compatibility
target_compile_options(CoreLibraryTestRunner PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -DCORE_LIBRARY_TESTING
    -fsanitize=address
    -fsanitize=leak
    -g
)

target_link_options(CoreLibraryTestRunner PRIVATE
    -fsanitize=address
    -fsanitize=leak
)

# Force linker to keep custom test section
if(APPLE)
    target_link_options(CoreLibraryTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:CoreLibraryTests>
    )
else()
    target_link_options(CoreLibraryTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Memory tracking is enabled by overriding allocation functions in MemoryTracker.cpp
# No special linker options needed - our definitions will override system ones

# Add subdirectory for kernel tests
add_subdirectory(kernel)

# Add a custom target to run tests
add_custom_target(run_tests
    COMMAND CoreLibraryTestRunner
    DEPENDS CoreLibraryTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS Core Library unit tests"
)

# Add a custom target to run all tests (core + kernel)
add_custom_target(run_all_tests
    COMMAND CoreLibraryTestRunner
    COMMAND kernel/KernelTestRunner
    DEPENDS CoreLibraryTestRunner kernel/KernelTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all CroCOS unit tests"
)