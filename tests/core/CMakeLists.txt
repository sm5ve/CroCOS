# CroCOS Kernel Unit Tests
# Created by Spencer Martin on 7/27/25.
# 
# This CMakeLists.txt tests kernel functionality using the host compiler
# with host-compatible compilation flags

cmake_minimum_required(VERSION 3.20)

# Create the core test library
add_library(CoreTests STATIC)

target_sources(CoreTests PRIVATE
    GraphTest.cpp
    HeapTest.cpp
    ObjectTest.cpp
    OptionalTest.cpp
    SmartPointerTest.cpp
    VariantTest.cpp
    VectorTest.cpp
)

# Add the TestHarness from parent directory
target_include_directories(CoreTests PUBLIC
    ..
    ../harness
    ../../libraries/Core/include
)

# Add corelib source files that we're testing
target_sources(CoreTests PRIVATE
    ../../libraries/Core/PrintStream.cpp
    ../../libraries/Core/str.cpp
    ../../libraries/Core/atomic/atomic.cpp
    ../../libraries/Core/Object.cpp
)

# Core tests use host-compatible compilation
target_compile_options(CoreTests PRIVATE
    -std=c++2c
    -fexceptions                    # Allow exceptions for test framework
    -Wall
    -Wextra
    -Wno-c++26-extensions          # Allow C++26 extensions
    -DCORE_KERNEL_TESTING          # Define to help resolve conflicts
    -DCORE_LINKED_WITH_KERNEL      # Match kernel compilation
    -DCORE_LIBRARY_TESTING
    -DCROCOS_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
    # Force include test.h to inject strong allocation symbols for objcopy
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../test.h
)

# Custom PRE_LINK command to instrument object files before linking
add_custom_command(TARGET CoreTests PRE_LINK
    COMMENT "Instrumenting corelib test object files for memory tracking before linking"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../instrument_objects.sh ${CMAKE_CURRENT_BINARY_DIR} CoreTests
)

# Create test executable
add_executable(CoreTestRunner)

target_sources(CoreTestRunner PRIVATE
    ../TestMain.cpp
)

target_link_libraries(CoreTestRunner PRIVATE
    CoreTests
    TestHarness
)

target_include_directories(CoreTestRunner PRIVATE
    ..
    ../harness
    ../../kernel/include
    ../../libraries/Core/include
)

# Test runner needs exceptions and standard library compatibility
target_compile_options(CoreTestRunner PRIVATE
    -std=c++2c
    -fexceptions
    -Wall
    -Wextra
    -DCORE_KERNEL_TESTING
    -O0
    -fsanitize=address
    -fsanitize=leak
    -g
)

# Force linker to keep custom test section
if(APPLE)
    target_link_options(CoreTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:CoreTests>
    )
else()
    target_link_options(CoreTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

target_compile_options(CoreTestRunner PRIVATE -fsanitize=address)
target_link_options(CoreTestRunner PRIVATE -fsanitize=address)

# On macOS, the linker automatically creates section boundary symbols
# On Linux, we need to tell the linker to keep our custom section
if(NOT APPLE)
    target_link_options(CoreTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests
    )
endif()

# Add a custom target to run kernel tests
add_custom_target(run_core_tests
    COMMAND CoreTestRunner
    DEPENDS CoreTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running CroCOS corelib unit tests"
)