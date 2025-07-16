set(CORE_SOURCES
        PrintStream.cpp
        str.cpp
        atomic/atomic.cpp
        include/core/atomic/RingBuffer.h
        include/core/Object.h
        include/core/algo/sort.h
        Object.cpp
        include/core/preprocessor.h
)

#Special version of Core meant to be linked with the kernel

add_library(CoreKernel STATIC ${CORE_SOURCES})

target_include_directories(CoreKernel PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

#target_compile_definitions(CoreKernel PRIVATE CORE_LINKED_WITH_KERNEL)

target_compile_options(CoreKernel PRIVATE -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -std=gnu++2b -masm=att
)

#Standard version of Core for userspace

add_library(CoreUser STATIC ${CORE_SOURCES})

target_include_directories(CoreUser PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(CoreUser PRIVATE
        -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings
        -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror
        -std=gnu++2a
)