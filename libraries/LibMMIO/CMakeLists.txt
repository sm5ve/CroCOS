set(MMIO_SOURCES
    Register.cpp
)

#Special version of Core meant to be linked with the kernel

add_library(MMIOKernel STATIC ${MMIO_SOURCES})

target_include_directories(MMIOKernel PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Core/include
)

target_compile_options(MMIOKernel PRIVATE -gdwarf-4)
target_compile_options(MMIOKernel PRIVATE -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -std=gnu++2b -masm=att
-DKERNEL -O0 -g)