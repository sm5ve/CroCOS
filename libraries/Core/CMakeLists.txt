add_library(Core STATIC
        PrintStream.cpp
        str.cpp
        atomic/atomic.cpp
        include/core/atomic/RingBuffer.h
)

#TODO change the flags depending on whether or not we're building for the kernel or userspace
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -std=gnu++2a -masm=att")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -masm=att")

target_include_directories(Core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)