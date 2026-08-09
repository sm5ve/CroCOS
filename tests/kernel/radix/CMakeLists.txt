# radix-tree userspace harness (Phases 0-2).
#
# Same shape as the vmsmalloc Phase 8 harness and for the same reason: the tree
# is compiled against REAL kernel headers with a mock VMSubstrate underneath, so
# what runs under ASan/TSan is the code that will run in the kernel, not a
# userspace re-implementation of it.
#
# Include-path ordering is load-bearing. tests/kernel/radix/mocks comes FIRST so
# <mem/VMSubstrate.h> resolves to the radix mock (the one carrying the DEC-052
# oracle); tests/kernel/vmsmalloc/mocks comes second and supplies everything the
# radix harness does not need to override — <mem/NUMA.h>, <kassert.h>,
# MockCpuLocal.h, MockKlog.h, MockInterruptContext.h — plus MockVMSubstrate.cpp
# itself, which the radix mock header declares against unchanged. Reordering
# these two lines silently swaps in the un-instrumented substrate and every
# lifetime test starts passing for the wrong reason.

set(RDX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(VMS_MOCKS ${CMAKE_CURRENT_SOURCE_DIR}/../vmsmalloc/mocks)

set(RDX_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TestMain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OracleTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GeometryTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CodecTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DispatchTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CoreTreeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DecompositionTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ModelTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OrderingSpellingTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ConcurrentTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DeferredReleaseTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RevalidationTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BucketCodecTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GrowthTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EnumerationTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PlacementTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AddressSpaceTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DescentCacheTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FreshnessTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/RadixOracle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/MockFreshness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RandomSource.cpp
    ${VMS_MOCKS}/MockVMSubstrate.cpp
    ${VMS_MOCKS}/MockKernelEnv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../rcu/mocks/MockRcuEnv.cpp
    ${RDX_ROOT}/kernel/rcu/RCU.cpp
    ${RDX_ROOT}/kernel/mm/vmsmalloc.cpp
    ${RDX_ROOT}/libraries/Core/PrintStream.cpp
    ${RDX_ROOT}/libraries/Core/str.cpp
    ${RDX_ROOT}/libraries/Core/Object.cpp
    ${RDX_ROOT}/libraries/Core/atomic/atomic.cpp
    ${RDX_ROOT}/libraries/Core/atomic/AtomicBitPool.cpp
)

# ITEM-084's draw histogram is ON here, and that is the point: the in-kernel
# stress unmaps exactly the span it placed — a FULL cover of one record — so it
# structurally cannot produce the multi-record partial cover ITEM-084 asks about.
# The harness can construct that shape deliberately.
add_compile_definitions(CROCOS_RADIX_DRAW_HISTOGRAM)

set(RDX_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks       # <mem/VMSubstrate.h> — MUST precede VMS_MOCKS
    ${VMS_MOCKS}                            # <mem/NUMA.h>, <kassert.h>, MockCpuLocal.h, ...
    ${CMAKE_CURRENT_SOURCE_DIR}/../vmsmalloc  # DebugIntrospection.h — vmsmalloc.cpp includes it
                                              # under CROCOS_VMSMALLOC_TEST_HARNESS
    ${CMAKE_CURRENT_SOURCE_DIR}/../rcu/mocks  # MockRcuEnv.h — the tree's domain needs it
    ${CMAKE_CURRENT_SOURCE_DIR}             # RadixHarness.h, RandomSource.h
    ${RDX_ROOT}/kernel/include
    ${RDX_ROOT}/kernel/mm                   # VMSubstrateSlab.h (real)
    ${RDX_ROOT}/libraries/Core/include
    ${RDX_ROOT}/libraries/LibAlloc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../harness
    ${CMAKE_CURRENT_SOURCE_DIR}/../..       # tests/ — assert_support.h, test.h, asan_poison.h
)

# The §11 ordering-spelling check is a SOURCE SCANNER — it needs the source.
set(RDX_HEADER_DIR ${RDX_ROOT}/kernel/include/mem/radix)

set(RDX_DEFINES
    CROCOS_RADIX_HEADER_DIR="${RDX_HEADER_DIR}"
    CROCOS_TESTING
    CORE_LIBRARY_TESTING
    CORE_KERNEL_TESTING
    CORE_LINKED_WITH_KERNEL
    CROCOS_VMSMALLOC_TEST_HARNESS
    CROCOS_RADIX_TEST_HARNESS
    CROCOS_RCU_TEST_HARNESS
)

set(RDX_COMMON_OPTS
    -std=c++2c -fexceptions -Wall -Wextra -Wno-c++26-extensions
    -include ${CMAKE_CURRENT_SOURCE_DIR}/../../assert_support.h)

# ─── ASan + leak runner ─────────────────────────────────────────────────────
# The ASan build is the one that carries the DEC-052 oracle: __asan_poison_*
# has no TSan equivalent, so the use-after-grace-period gate lives here and
# skips itself (loudly) on the TSan runner.
add_executable(KernelRadixTestRunner ${RDX_SOURCES})
target_include_directories(KernelRadixTestRunner PRIVATE ${RDX_INCLUDES})
target_compile_definitions(KernelRadixTestRunner PRIVATE ${RDX_DEFINES})
target_compile_options(KernelRadixTestRunner PRIVATE
    ${RDX_COMMON_OPTS} -O0 -g -fsanitize=address -fsanitize=leak)
target_link_libraries(KernelRadixTestRunner PRIVATE TestHarness)
target_link_options(KernelRadixTestRunner PRIVATE -fsanitize=address)
if(APPLE)
    target_link_options(KernelRadixTestRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>)
else()
    target_link_options(KernelRadixTestRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_radix_tests
    COMMAND KernelRadixTestRunner
    DEPENDS KernelRadixTestRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running radix-tree tests (ASan/leak)")

# ─── TSan runner (ARMv8/M1 = primary release gate) ──────────────────────────
# Stood up from Phase 0 even though Phase 0 is single-threaded: the runner is
# far cheaper to build now than to retrofit alongside Phase 2's claim protocol,
# and "the TSan build exists" is itself a Phase 0 exit criterion.
add_executable(KernelRadixTestRunnerTSan ${RDX_SOURCES})
target_include_directories(KernelRadixTestRunnerTSan PRIVATE ${RDX_INCLUDES})
target_compile_definitions(KernelRadixTestRunnerTSan PRIVATE ${RDX_DEFINES})
target_compile_options(KernelRadixTestRunnerTSan PRIVATE
    ${RDX_COMMON_OPTS} -O1 -g -fsanitize=thread)
target_link_libraries(KernelRadixTestRunnerTSan PRIVATE TestHarness)
target_link_options(KernelRadixTestRunnerTSan PRIVATE -fsanitize=thread)
if(APPLE)
    target_link_options(KernelRadixTestRunnerTSan PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>
        -Wl,-undefined,dynamic_lookup)
else()
    target_link_options(KernelRadixTestRunnerTSan PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_radix_tests_tsan
    COMMAND KernelRadixTestRunnerTSan
    DEPENDS KernelRadixTestRunnerTSan
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running radix-tree tests (TSan)")

# ─── Progress-audit runner (§11's maximum-holder assert) ────────────────────
# §6.7's maximum-holder lemma is the only assertable progress property, and
# asserting it needs each writer to publish what it holds so a failing writer can
# tell a writer-held bit from a transient phantom. That publishing is two epoch
# bumps and two stores per claim acquired, on the hottest path in the protocol —
# a verification instrument, not a production one — so it gets its own target
# rather than slowing the ASan and TSan gates.
#
# TSan rather than ASan: the property is about writers racing, and the audit's
# own seqlock-shaped epoch window is exactly the kind of thing TSan should be
# looking at.
add_executable(KernelRadixProgressAuditRunner ${RDX_SOURCES})
target_include_directories(KernelRadixProgressAuditRunner PRIVATE ${RDX_INCLUDES})
target_compile_definitions(KernelRadixProgressAuditRunner PRIVATE
    ${RDX_DEFINES} CROCOS_RADIX_PROGRESS_AUDIT)
target_compile_options(KernelRadixProgressAuditRunner PRIVATE
    ${RDX_COMMON_OPTS} -O1 -g -fsanitize=thread)
target_link_libraries(KernelRadixProgressAuditRunner PRIVATE TestHarness)
target_link_options(KernelRadixProgressAuditRunner PRIVATE -fsanitize=thread)
if(APPLE)
    target_link_options(KernelRadixProgressAuditRunner PRIVATE
        -Wl,-force_load,$<TARGET_FILE:TestHarness>
        -Wl,-undefined,dynamic_lookup)
else()
    target_link_options(KernelRadixProgressAuditRunner PRIVATE
        -Wl,--keep-section=.crocos_unit_tests)
endif()

add_custom_target(run_radix_progress_audit
    COMMAND KernelRadixProgressAuditRunner
    DEPENDS KernelRadixProgressAuditRunner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running radix-tree tests (TSan + §6.7 maximum-holder audit)")
