# vmsmalloc Phase 8 — userspace integration test harness.
#
# Compiles the production kernel/mm/vmsmalloc.cpp against mock headers (mocks/
# shadows <mem/VMSubstrate.h>, <mem/NUMA.h>, <kassert.h> via include-path
# order) plus the real VMSubstrateSlab.h / CpuLocal.h / InterruptContextDepths.h
# (which compile under CROCOS_TESTING). ASan/leak runner; the TSan runner is a
# follow-up increment.

set(VMS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

set(VMS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TestMain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/IntegrationTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConcurrentTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AssertionsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/MockVMSubstrate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/MockKernelEnv.cpp
    ${VMS_ROOT}/kernel/mm/vmsmalloc.cpp
    ${VMS_ROOT}/libraries/Core/PrintStream.cpp
    ${VMS_ROOT}/libraries/Core/str.cpp
    ${VMS_ROOT}/libraries/Core/Object.cpp
    ${VMS_ROOT}/libraries/Core/atomic/atomic.cpp
    ${VMS_ROOT}/libraries/Core/atomic/AtomicBitPool.cpp
)

set(VMS_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks          # shadow mem/VMSubstrate.h, mem/NUMA.h, kassert.h
    ${CMAKE_CURRENT_SOURCE_DIR}                # DebugIntrospection.h
    ${VMS_ROOT}/kernel/include
    ${VMS_ROOT}/kernel/mm                      # VMSubstrateSlab.h (real)
    ${VMS_ROOT}/libraries/Core/include
    ${VMS_ROOT}/libraries/LibAlloc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../harness  # TestHarness.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../..          # tests/ — assert_support.h, test.h
)

set(VMS_DEFINES
    CROCOS_TESTING
    CORE_LIBRARY_TESTING
    CORE_KERNEL_TESTING
    CORE_LINKED_WITH_KERNEL
    CROCOS_VMSMALLOC_TEST_HARNESS
)

# ─── ASan + leak runner ─────────────────────────────────────────────────────
add_executable(KernelVmsmallocIntegrationTestRunner ${VMS_SOURCES})
target_include_directories(KernelVmsmallocIntegrationTestRunner PRIVATE ${VMS_INCLUDES})
target_compile_definitions(KernelVmsmallocIntegrationTestRunner PRIVATE ${VMS_DEFINES})
target_compile_options(KernelVmsmallocIntegrationTestRunner PRIVATE
    -std=c++2c -fexceptions -Wall -Wextra -Wno-c++26-extensions
    -O0 -g -fsanitize=address -fsanitize=leak
    # Force-include assert_support.h so the throwing assert macro (Core's
    # <assert.h> / the mock <kassert.h>) sees CroCOSTest declared in every TU,
    # including the mocks and vmsmalloc.cpp (which don't include test.h).
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../assert_support.h)
target_link_libraries(KernelVmsmallocIntegrationTestRunner PRIVATE TestHarness)
target_link_options(KernelVmsmallocIntegrationTestRunner PRIVATE -fsanitize=address)
if(APPLE)
    target_link_options(KernelVmsmallocIntegrationTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>)
else()
    target_link_options(KernelVmsmallocIntegrationTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_vmsmalloc_integration_tests
    COMMAND KernelVmsmallocIntegrationTestRunner
    DEPENDS KernelVmsmallocIntegrationTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running vmsmalloc integration tests (ASan/leak)")

# ─── TSan runner (ARMv8/M1 = primary release gate, P8-DEC-004) ──────────────
# Same sources compiled with -fsanitize=thread. TestHarness links uninstrumented
# (-undefined,dynamic_lookup on Apple, mirroring the Phase-1/4 TSan runners).
add_executable(KernelVmsmallocIntegrationTestRunnerTSan ${VMS_SOURCES})
target_include_directories(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE ${VMS_INCLUDES})
target_compile_definitions(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE ${VMS_DEFINES})
target_compile_options(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE
    -std=c++2c -fexceptions -Wall -Wextra -Wno-c++26-extensions
    -O1 -g -fsanitize=thread
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../assert_support.h)
target_link_libraries(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE TestHarness)
target_link_options(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE -fsanitize=thread)
if(APPLE)
    target_link_options(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>
        -Wl,-undefined,dynamic_lookup)
else()
    target_link_options(KernelVmsmallocIntegrationTestRunnerTSan PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_vmsmalloc_integration_tests_tsan
    COMMAND KernelVmsmallocIntegrationTestRunnerTSan
    DEPENDS KernelVmsmallocIntegrationTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running vmsmalloc integration tests (TSan)")
