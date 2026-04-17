# CroCOS Page Allocator Stress Tests
# Standalone CMake project — separate from unit tests.
# No ASAN/LSAN; optimized at -O2 for realistic throughput numbers.
#
# Build:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Run:
#   ./build/PageAllocatorStress [options]

cmake_minimum_required(VERSION 3.20)

find_program(HOMEBREW_CLANG   /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    set(CMAKE_C_COMPILER   ${HOMEBREW_CLANG})
    set(CMAKE_CXX_COMPILER ${HOMEBREW_CLANGXX})
    message(STATUS "Using Homebrew clang: ${HOMEBREW_CLANG}")
else()
    set(CMAKE_C_COMPILER   clang)
    set(CMAKE_CXX_COMPILER clang++)
    message(STATUS "Using system clang (fallback)")
endif()

project(CroCOSStressTests LANGUAGES CXX)

# ---- Kernel and library sources ----
set(KERNEL_SOURCES
    ../kernel/mm/NewPageAllocator.cpp
    ../kernel/mm/MemTypes.cpp
    ../kernel/mm/NUMA.cpp
    ../kernel/arch/arch.cpp
)

set(CORE_SOURCES
    ../libraries/Core/PrintStream.cpp
    ../libraries/Core/str.cpp
    ../libraries/Core/Object.cpp
    ../libraries/Core/atomic/atomic.cpp
    ../libraries/Core/atomic/AtomicBitPool.cpp
)

add_executable(PageAllocatorStress
    StressMocks.cpp
    PageAllocatorStress.cpp
    ${KERNEL_SOURCES}
    ${CORE_SOURCES}
)

target_include_directories(PageAllocatorStress PRIVATE
    ../kernel/include
    ../libraries/Core/include
)

# Shared flags applied to all sources in this target
target_compile_options(PageAllocatorStress PRIVATE
    -std=c++2c
    -O2
    -Wall
    -Wextra
    -Wno-c++26-extensions
    -Wno-unused-parameter
    -DCORE_KERNEL_TESTING
    -DCORE_LINKED_WITH_KERNEL
    -DCORE_LIBRARY_TESTING
    -DCROCOS_TESTING
    -g
)

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    target_link_libraries(PageAllocatorStress PRIVATE
        /opt/homebrew/opt/llvm/lib/c++/libc++.a
        /opt/homebrew/opt/llvm/lib/c++/libc++abi.a
    )
else()
    target_link_libraries(PageAllocatorStress PRIVATE c++)
endif()

# Force-include StressAssertSupport.h into every TU.
# PageTableSpecification.h unconditionally defines PARANOID_PAGING_ASSERTIONS and
# uses assert(), which expands to CroCOSTest::AssertionFailure under
# CORE_LIBRARY_TESTING.  Every compilation unit that touches arch.h needs
# CroCOSTest in scope, so we apply the force-include target-wide.
target_compile_options(PageAllocatorStress PRIVATE
    -include${CMAKE_CURRENT_SOURCE_DIR}/StressAssertSupport.h
)

target_link_options(PageAllocatorStress PRIVATE -pthread)

add_custom_target(run_stress
    COMMAND PageAllocatorStress
    DEPENDS PageAllocatorStress
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running page allocator stress test (Ctrl+C to stop)"
)
