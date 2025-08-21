# CroCOS Kernel Unit Tests
# Created by Spencer Martin on 7/27/25.
# 
# This CMakeLists.txt tests kernel functionality using the host compiler
# with host-compatible compilation flags

cmake_minimum_required(VERSION 3.20)

# Create the kernel test library 
add_library(LibAllocTests STATIC)

target_sources(LibAllocTests PRIVATE
        InternalAllocTest.cpp
)

# Add the TestHarness from parent directory
target_include_directories(LibAllocTests PUBLIC
    ../harness
    ../../libraries/LibAlloc/include
    ../../libraries/Core/include
)

# Add liballoc source files that we're testing
target_sources(LibAllocTests PRIVATE
    ../../libraries/LibAlloc/InternalAllocator.cpp
    ../../libraries/LibAlloc/backends/UnitTests.cpp
)

# Kernel tests use host-compatible compilation but with freestanding mode for tested code
target_compile_options(LibAllocTests PRIVATE
    -std=c++2c
    -fexceptions                    # Allow exceptions for test framework
    -Wall
    -Wextra
    -Wno-c++26-extensions          # Allow C++26 extensions
    -DCROCOS_TESTING
    -DCORE_LIBRARY_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
    # Force include test.h to inject strong allocation symbols for objcopy
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h
)

# Custom PRE_LINK command to instrument object files before linking
add_custom_command(TARGET LibAllocTests PRE_LINK
    COMMENT "Instrumenting kernel test object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} LibAllocTests
)

# Create test executable
add_executable(LibAllocTestRunner)

target_sources(LibAllocTestRunner PRIVATE
    ../TestMain.cpp
)

target_link_libraries(LibAllocTestRunner PRIVATE
    LibAllocTests
    TestHarness
)

target_include_directories(LibAllocTestRunner PRIVATE
    ..
    ../harness
)

# Test runner needs exceptions and standard library compatibility
target_compile_options(LibAllocTestRunner PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -DCORE_KERNEL_TESTING
    -DCORE_LIBRARY_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
)

# Force linker to keep custom test section
if(APPLE)
    target_link_options(LibAllocTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:LibAllocTests>
    )
    target_link_options(LibAllocTestRunner PRIVATE
            -Wl,-undefined,dynamic_lookup  # macOS: allow undefined symbols to be resolved at runtime
    )
else()
    target_link_options(LibAllocTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

target_compile_options(LibAllocTestRunner PRIVATE -fsanitize=address)
target_link_options(LibAllocTestRunner PRIVATE -fsanitize=address)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(LibAllocTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Add a custom target to run kernel tests
add_custom_target(run_liballoc_tests
    COMMAND LibAllocTestRunner
    DEPENDS LibAllocTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS LibAlloc unit tests"
)