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
        arch/arch.cpp
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
        interrupts/InterruptGraphManager.cpp
        cxxcompat.cpp
        arch/amd64/interrupts/APIC.cpp
        arch/amd64/timers/PIT.cpp
        arch/amd64/timers/HPET.cpp
        interrupts/InterruptRoutingAndDispatch.cpp
        interrupts/InterruptRoutingPolicyImpls.cpp
        interrupts/ExplicitTypes.cpp
        timing/BlockingSleep.cpp
        arch/amd64/smp/smpboot.asm
        init.cpp
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated_headers)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated_source)

set_source_files_properties(
        InterruptGraphManager.cpp
        InterruptRoutingAndDispatch.cpp
        interrupts/InterruptRoutingPolicyImpls.cpp
        PROPERTIES COMPILE_FLAGS "-O2 -ftemplate-backtrace-limit=0"
)

#We need to be smarter to split out architecture-specific source files
file(GLOB_RECURSE COMMON_INIT_FILES
        "${CMAKE_SOURCE_DIR}/kernel/*.icd"
)

set(INIT_DSL_FILES ${COMMON_INIT_FILES})

# Define output location
set(GENERATED_INIT_CPP "${CMAKE_BINARY_DIR}/generated_source/init_registry.cpp")

# Custom command to generate the header
add_custom_command(
        OUTPUT ${GENERATED_INIT_CPP}
        COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/gen_init_registry.py"
        -o ${GENERATED_INIT_CPP}
        ${INIT_DSL_FILES}
        DEPENDS
        ${INIT_DSL_FILES}
        "${CMAKE_SOURCE_DIR}/tools/gen_init_registry.py"
        "${CMAKE_SOURCE_DIR}/tools/toollib.py"  # If you have helper modules
        COMMENT "Generating initialization registry from ICD files"
        VERBATIM
)

# Create a custom target for the generated header
add_custom_target(generate_init_registry
        DEPENDS ${GENERATED_INIT_HEADER}
)

add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/generated_headers/isr.inc, ${CMAKE_BINARY_DIR}/generated_headers/isr_set.inc
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_idt_install_macros.py
        -o ${CMAKE_BINARY_DIR}/generated_headers/
        DEPENDS ${INIT_DEF_FILES} ${CMAKE_SOURCE_DIR}/tools/gen_idt_install_macros.py
        COMMENT "Generating init registry"
)

add_custom_target(generate_idt_macros
    DEPENDS ${CMAKE_BINARY_DIR}/generated_headers/isr.inc, ${CMAKE_BINARY_DIR}/generated_headers/isr_set.inc
)

add_executable(Kernel ${KERNEL_SRC} ${GENERATED_INIT_CPP})
add_dependencies(Kernel generate_idt_macros generate_init_registry)
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
target_include_directories(Kernel PRIVATE ${CMAKE_BINARY_DIR}/generated_headers)
target_link_libraries(Kernel PRIVATE CoreKernel AllocKernel MMIOKernel gcc)

add_custom_command(TARGET Kernel POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:Kernel> $<TARGET_FILE:Kernel>.debug
        COMMAND ${CMAKE_OBJCOPY} --strip-debug --add-gnu-debuglink=$<TARGET_FILE:Kernel>.debug $<TARGET_FILE:Kernel>
        COMMENT "Stripping symbols and creating debug file with build ID"
)