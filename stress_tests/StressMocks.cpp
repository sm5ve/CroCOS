// StressMocks.cpp
// Lightweight kernel/arch stubs for the standalone stress test harness.
// No dependency on the unit-test framework (TestHarness, MemoryTracker, etc.).

#include <cstdio>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <atomic>

#include <arch.h>
#include <kernel.h>
#include <core/PrintStream.h>

// ============================================================================
// Streams
// ============================================================================

namespace {
    class NullStream final : public Core::PrintStream {
    protected:
        void putString(const char*) override {}
    };

    class StderrStream final : public Core::PrintStream {
    protected:
        void putString(const char* s) override { fputs(s, stderr); }
    };

    NullStream   gNullStream;
    StderrStream gStderrStream;
}

// ============================================================================
// kernel:: stubs
// ============================================================================

namespace kernel {
    Core::AtomicPrintStream klog() {
        return Core::AtomicPrintStream(gNullStream);
    }

    Core::PrintStream& emergencyLog() { return gStderrStream; }

    // Panic support — print to stderr then abort so the test harness catches it.
    void print_stacktrace() {}
    void print_stacktrace(uintptr_t*) {}
}

// ============================================================================
// arch:: mocks
// ============================================================================
// Each new thread gets a CPU ID assigned round-robin up to processorCount().
// IDs are cached thread-locally to avoid lock contention on the hot path.

namespace arch {
    namespace {
        std::mutex              gIdMutex;
        std::unordered_map<std::thread::id, ProcessorID> gThreadMap;
        std::atomic<uint32_t>   gNextId{0};
        size_t                  gProcessorCount = 8;

        thread_local ProcessorID tCachedId  = 255;
        thread_local bool        tIdCached  = false;
    }

    // Called by the stress test before spawning worker threads.
    void stressSetProcessorCount(size_t n) {
        std::lock_guard lock(gIdMutex);
        gProcessorCount = n;
    }

    ProcessorID getCurrentProcessorID() {
        if (tIdCached) return tCachedId;

        std::lock_guard lock(gIdMutex);
        auto it = gThreadMap.find(std::this_thread::get_id());
        if (it != gThreadMap.end()) {
            tCachedId = it->second;
            tIdCached = true;
            return tCachedId;
        }

        auto pid = static_cast<ProcessorID>(gNextId.fetch_add(1) % gProcessorCount);
        gThreadMap[std::this_thread::get_id()] = pid;
        tCachedId = pid;
        tIdCached = true;
        return pid;
    }

    size_t processorCount()   { return gProcessorCount; }
    size_t getCacheLineSize() { return 64; }
}
