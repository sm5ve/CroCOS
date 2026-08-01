# Specify the target system architecture
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# CroCOS is built with its own x86_64-crocos cross toolchain, not a stock
# x86_64-elf one. See tools/toolchain/README.md for why -- briefly, a stock
# cross GCC emits global constructors into legacy .ctors sections that the
# kernel's linker script does not collect, so they silently never run.
#
# Build it with tools/toolchain/build-toolchain.sh. Look for it in the default
# install prefix first so that no PATH setup is needed; -DCROCOS_TOOLCHAIN_PREFIX
# overrides, and if neither yields a compiler we fall back to a bare PATH lookup
# so an installation somewhere else still works.
set(CROCOS_TOOLCHAIN_PREFIX "$ENV{HOME}/opt/crocos-toolchain"
    CACHE PATH "Install prefix of the x86_64-crocos cross toolchain")

set(CROCOS_TARGET_TRIPLE x86_64-crocos)

if(EXISTS "${CROCOS_TOOLCHAIN_PREFIX}/bin/${CROCOS_TARGET_TRIPLE}-gcc")
    set(_crocos_tool_prefix "${CROCOS_TOOLCHAIN_PREFIX}/bin/${CROCOS_TARGET_TRIPLE}-")
else()
    set(_crocos_tool_prefix "${CROCOS_TARGET_TRIPLE}-")
endif()

# Set the cross-compiler binaries
set(CMAKE_C_COMPILER   ${_crocos_tool_prefix}gcc)
set(CMAKE_CXX_COMPILER ${_crocos_tool_prefix}g++)

# Set the assembler and linker
set(CMAKE_ASM_COMPILER ${_crocos_tool_prefix}gcc)
set(CMAKE_LINKER       ${_crocos_tool_prefix}ld)

# Ensure that the cross-compiler does not add platform-specific flags (e.g., -arch for macOS)
string(REPLACE "-arch" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-arch" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Tell CMake to use the specified cross-compiler tools instead of the native ones
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
