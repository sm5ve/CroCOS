set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/toolchain.cmake)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

cmake_minimum_required(VERSION 3.20)
project(CroCOS LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_CXX_COMPILER x86_64-elf-g++)
set(CMAKE_ASM_COMPILER x86_64-elf-gcc)

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
        )

add_dependencies(run Kernel)

# Add unit test target that builds and runs Core library tests
add_custom_target(unit_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
        COMMAND cmake -B build -DCMAKE_BUILD_TYPE=Debug
        COMMAND cmake --build build
        COMMAND ./build/CoreLibraryTests
        COMMENT "Building and running CroCOS Core library unit tests"
        )
#add_executable(CroCOS)
#target_link_libraries(CroCOS PRIVATE Kernel)