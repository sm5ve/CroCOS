#TODO enable SSE and SSE2, and remove -mno-sse(2)
set(CMAKE_ASM_FLAGS "-ffreestanding -mcmodel=kernel  -fno-exceptions -nostdlib -Wall -Wextra -x assembler-with-cpp -masm=att")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -std=gnu++2a -masm=att")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -masm=att")


set(KERNEL_SRC
        arch/amd64/Amd64Init.cpp
        arch/amd64/InstructionWrappers.cpp
        mm/PageAllocator.cpp
        arch/amd64/SerialPort.cpp
        arch/amd64/Spinlock.cpp
        arch/amd64/multiboot1.asm
        arch/amd64/interrupts/InterruptVectorTrampolines.asm
        KernelMain.cpp
        arch/hal/hal.cpp
        include/arch/hal/hal.h
        include/kernel.h mm/BumpAllocator.cpp
        panic.cpp
        mm/kmalloc.cpp
        mm/allocators.h
        include/acpi.h
        include/mm.h
        acpi/ACPIFixedTables.cpp
        include/assert.h
        include/panic.h
        mm/MemoryManager.cpp
        arch/amd64/memops.cpp
        include/arch/hal/spinlock.h
        arch/amd64/PageTableManager.cpp
        include/FlushPlanner.h
        timing/TimerManager.cpp
        include/timers.h
        include/arch/hal/interrupts.h
        arch/amd64/interrupts/APIC.cpp
        include/arch/amd64/interrupts/APIC.h
        arch/amd64/interrupts/LegacyPIC.cpp
        include/arch/amd64/interrupts/LegacyPIC.h
        arch/amd64/smp/smp.cpp
        include/arch/amd64/smp.h
        arch/amd64/interrupts/InterruptSetup.cpp
        arch/amd64/lgdt.asm
        arch/hal/InterruptManager.cpp
        ../libraries/Core/include/core/atomic.h
)

execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=crtbegin.o
        OUTPUT_VARIABLE CRTBEGIN_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
        COMMAND ${CMAKE_C_COMPILER} -print-file-name=crtend.o
        OUTPUT_VARIABLE CRTEND_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_executable(Kernel ${KERNEL_SRC})
target_link_options(Kernel PUBLIC -O0 -ffreestanding -mcmodel=kernel -nostdlib -T ${CMAKE_SOURCE_DIR}/kernel/linker.ld)
target_include_directories(Kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Kernel PRIVATE Core)