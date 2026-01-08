#TODO enable SSE and SSE2, and remove -mno-sse(2)
set(CMAKE_ASM_FLAGS "-ffreestanding -mcmodel=kernel  -fno-exceptions -nostdlib -Wall -Wextra -x assembler-with-cpp -masm=att")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fimplicit-constexpr -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel -masm=att -fconcepts-diagnostics-depth=3 -DKERNEL")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel  -masm=att")


set(KERNEL_SRC
        arch/amd64/Amd64Init.cpp
        arch/amd64/InstructionWrappers.cpp
        mm/PageAllocator.cpp
        arch/amd64/SerialPort.cpp
        arch/amd64/multiboot1.asm
        arch/amd64/interrupts/InterruptVectorTrampolines.asm
        KernelMain.cpp
        arch/hal/hal.cpp
        panic.cpp
        mm/kmalloc.cpp
        acpi/ACPIFixedTables.cpp
        mm/MemoryManager.cpp
        arch/amd64/PageTableManager.cpp
        timing/TimerQueues.cpp
        timing/ClockManager.cpp
        arch/amd64/interrupts/LegacyPIC.cpp
        arch/amd64/smp/smp.cpp
        arch/amd64/interrupts/InterruptSetup.cpp
        arch/amd64/lgdt.asm
        arch/hal/InterruptGraphManager.cpp
        cxxcompat.cpp
        arch/amd64/interrupts/APIC.cpp
        arch/amd64/timers/PIT.cpp
        arch/amd64/timers/HPET.cpp
        arch/hal/InterruptRoutingAndDispatch.cpp
        arch/hal/InterruptRoutingPolicyImpls.cpp
        arch/hal/ExplicitTypes.cpp
        timing/BlockingSleep.cpp
)

set_source_files_properties(
        InterruptGraphManager.cpp
        InterruptRoutingAndDispatch.cpp
        arch/hal/InterruptRoutingPolicyImpls.cpp
        PROPERTIES COMPILE_FLAGS "-O2 -ftemplate-backtrace-limit=0"
)

add_executable(Kernel ${KERNEL_SRC})
target_compile_options(Kernel PRIVATE -gdwarf-4)
target_compile_options(Kernel PRIVATE -ffunction-sections -fdata-sections)
target_link_options(Kernel PUBLIC
        -flto -Os -ffreestanding -mcmodel=kernel -nostdlib
        -T ${CMAKE_SOURCE_DIR}/kernel/linker.ld
        -Wl,--gc-sections
        #-Wl,--print-gc-sections
        -Wl,--build-id
)
target_include_directories(Kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(Kernel PRIVATE CoreKernel AllocKernel MMIOKernel gcc)

add_custom_command(TARGET Kernel POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:Kernel> $<TARGET_FILE:Kernel>.debug
        COMMAND ${CMAKE_OBJCOPY} --strip-debug --add-gnu-debuglink=$<TARGET_FILE:Kernel>.debug $<TARGET_FILE:Kernel>
        COMMENT "Stripping symbols and creating debug file with build ID"
)