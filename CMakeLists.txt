# Use Homebrew clang with C++26 support, fallback to system clang
find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/toolchain.cmake)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

cmake_minimum_required(VERSION 3.20)
project(CroCOS LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_BUILD -O0 -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_BUILD -O0 -fno-omit-frame-pointer")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
endif()

add_subdirectory(libraries)
add_subdirectory(kernel)

add_custom_target(run
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMAND qemu-system-x86_64 -kernel kernel/Kernel -no-reboot -nographic -smp 4 -m 256M -d guest_errors -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU"
        USES_TERMINAL
)

add_dependencies(run Kernel)

# Add unit test target that builds and runs all tests
add_custom_target(unit_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
        COMMAND cmake -B build -DCMAKE_BUILD_TYPE=Debug
        COMMAND cmake --build build --target run_all_tests
        COMMENT "Building and running all CroCOS unit tests"
        )
#add_executable(CroCOS)
#target_link_libraries(CroCOS PRIVATE Kernel)