//
// Created by Spencer Martin on 2/12/26.
//

#include "../test.h"
#include "TestHarness.h"
#include <arch.h>
#include <kernel.h>
#include <thread>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include "ArchMocks.h"
#include <assert.h>
#include "MemoryTracker.h"

// ============================================================================
// kernel::klog() stub — routes to Core::cout() for test visibility
// ============================================================================

namespace kernel {
    Core::AtomicPrintStream klog() {
        return Core::AtomicPrintStream(Core::cout());
    }
}

namespace arch {
    namespace {
        // Which logical CPU this thread is running as. Unbound is the default:
        // a thread that never declared a CPU has no business reading per-CPU
        // kernel state, and saying so loudly beats inventing an ID for it.
        thread_local bool        tlBound   = false;
        thread_local ProcessorID tlBoundCpu = 0;

        constexpr size_t kMaxMockProcessors = 256;   // arch::MAX_PROCESSOR_COUNT

        // Exclusivity ledger. Guards the invariant the whole model exists for:
        // at most one live thread per logical CPU. Indexed by ProcessorID; a
        // plain array so binding costs no allocation inside a stress loop.
        std::mutex        bindingMutex;
        std::thread::id   holder[kMaxMockProcessors];
        bool              held[kMaxMockProcessors] = {};

        size_t mockProcessorCount = 8;
    }

    namespace testing {
        void bindCurrentThread(ProcessorID id) {
            std::lock_guard<std::mutex> lock(bindingMutex);
            assert(id < mockProcessorCount,
                "bound to a logical CPU outside the configured processor count");
            // Exclusivity: the ID must be free, unless this very thread holds it.
            if (held[id]) {
                assert(holder[id] == std::this_thread::get_id(),
                    "two live threads bound to one logical CPU — per-CPU state is "
                    "not exclusive; give each worker its own ProcessorBinding, and "
                    "wrap the spawning thread in a DetachedDriver while it waits");
            }
            if (tlBound && tlBoundCpu != id) {
                held[tlBoundCpu] = false;
            }
            held[id]   = true;
            holder[id] = std::this_thread::get_id();
            tlBound    = true;
            tlBoundCpu = id;
        }

        void unbindCurrentThread() {
            std::lock_guard<std::mutex> lock(bindingMutex);
            if (tlBound) {
                if (held[tlBoundCpu] && holder[tlBoundCpu] == std::this_thread::get_id())
                    held[tlBoundCpu] = false;
                tlBound = false;
            }
        }

        void resetProcessorState() {
            std::lock_guard<std::mutex> lock(bindingMutex);
            for (size_t i = 0; i < kMaxMockProcessors; ++i) held[i] = false;
            tlBound = false;
        }

        void setProcessorCount(size_t count) {
            std::lock_guard<std::mutex> lock(bindingMutex);
            assert(count > 0 && count <= kMaxMockProcessors,
                "mock processor count out of range");
            // A live binding outside the new range would silently become an
            // out-of-range index into every per-CPU array sized from this count.
            for (size_t i = count; i < kMaxMockProcessors; ++i) {
                assert(!held[i],
                    "processor count lowered while a thread is bound above it");
            }
            mockProcessorCount = count;
        }

        size_t getProcessorCount() {
            std::lock_guard<std::mutex> lock(bindingMutex);
            return mockProcessorCount;
        }

        ProcessorBinding::ProcessorBinding(ProcessorID id)
            : previous(tlBoundCpu), hadPrevious(tlBound) {
            bindCurrentThread(id);
        }

        ProcessorBinding::~ProcessorBinding() {
            unbindCurrentThread();
            if (hadPrevious) bindCurrentThread(previous);
        }

        DetachedDriver::DetachedDriver()
            : previous(tlBoundCpu), hadPrevious(tlBound) {
            unbindCurrentThread();
        }

        DetachedDriver::~DetachedDriver() {
            if (hadPrevious) bindCurrentThread(previous);
        }
    }

    ProcessorID getCurrentProcessorID() {
        // No silent fallback. A thread reaching per-CPU kernel state without
        // declaring which CPU it is running as is the bug, not an inconvenience.
        assert(tlBound,
            "getCurrentProcessorID() on a thread with no ProcessorBinding");
        return tlBoundCpu;
    }

    size_t processorCount() {
        return mockProcessorCount;
    }

    size_t getCacheLineSize() {
        return 64;
    }
}

// ============================================================================
// Per-test hooks
// ============================================================================

// Every test body runs as CPU 0 unless it says otherwise, so single-threaded
// tests need no ceremony. Runs on the body thread — which for a timed test is
// a freshly spawned one, not main.
static void archMocksPrologue() {
    arch::testing::bindCurrentThread(0);
}

static void archMocksCleanup() {
    arch::testing::resetProcessorState();
}

REGISTER_TEST_PROLOGUE(archMocksPrologue)
REGISTER_TEST_CLEANUP(archMocksCleanup)
