# Specify the target system architecture
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Set the cross-compiler binaries
set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_CXX_COMPILER x86_64-elf-g++)

# Set the assembler and linker
set(CMAKE_ASM_COMPILER x86_64-elf-as)
set(CMAKE_LINKER x86_64-elf-ld)

# Ensure that the cross-compiler does not add platform-specific flags (e.g., -arch for macOS)
string(REPLACE "-arch" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-arch" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Tell CMake to use the specified cross-compiler tools instead of the native ones
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)