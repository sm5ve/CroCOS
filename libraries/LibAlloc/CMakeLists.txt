set(MALLOC_SOURCES
        InternalAllocator.cpp
        backends/Kernel.cpp
        SlabAllocator.cpp
        LibAllocMain.cpp
)

#Special version of Core meant to be linked with the kernel

add_library(AllocKernel STATIC ${MALLOC_SOURCES})

target_include_directories(AllocKernel PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Core/include
)

#target_compile_definitions(CoreKernel PRIVATE CORE_LINKED_WITH_KERNEL)

target_compile_options(AllocKernel PRIVATE -gdwarf-4)
target_compile_options(AllocKernel PRIVATE -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -std=gnu++2b -masm=att
-DKERNEL -O3 -g)

#Standard version of Core for userspace

#add_library(MallocUser STATIC ${CORE_SOURCES})

#target_include_directories(MallocUser PUBLIC
#        ${CMAKE_CURRENT_SOURCE_DIR}/include
#)

#target_compile_options(MallocUser PRIVATE
#        -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings
#        -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror
#        -std=gnu++2a
#)