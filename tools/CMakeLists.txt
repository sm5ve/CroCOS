cmake_minimum_required(VERSION 3.20)

# Use Homebrew clang with C++26 support, fallback to system clang
find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

set(CMAKE_ASM_FLAGS "-Wall -Wextra -x")

if(HOMEBREW_CLANG AND HOMEBREW_CLANGXX)
    set(CMAKE_C_COMPILER ${HOMEBREW_CLANG})
    set(CMAKE_CXX_COMPILER ${HOMEBREW_CLANGXX})
    message(STATUS "Using Homebrew clang: ${HOMEBREW_CLANG}")
else()
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    message(STATUS "Using system clang (fallback)")
endif()

project(CroCOSBuildTools LANGUAGES CXX)

add_executable(generate_linker_script DeriveKernelLayout.cpp)
target_include_directories(generate_linker_script PRIVATE ../kernel/include ../libraries/Core/include)

target_compile_options(generate_linker_script PRIVATE
        -std=c++2c
        -Wall
        -Wextra
        -g
        -DHOSTED
        -DARCH_OVERRIDE
        -DARCH_AMD64
)