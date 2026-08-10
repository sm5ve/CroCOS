//
// Tests for the arch mock's logical-CPU model.
//
// These cover the mock itself rather than any kernel code, because the mock now
// carries an invariant the kernel depends on and cannot check for itself: at
// most one live thread per logical CPU. That invariant was previously absent —
// the mock assigned IDs round-robin modulo a count the fixture never set, which
// put two live threads on one CPU ~170 times per suite run and, when the count
// disagreed with the allocator's pool count, indexed past `localPools` and
// SEGV'd. A guard nobody tests is a guard nobody can rely on, so each assertion
// below is exercised in the direction that should fire.
//

#include "../test.h"
#include <TestHarness.h>

#include "ArchMocks.h"
#include <arch.h>

#include <thread>
#include <vector>
#include <atomic>

namespace {
    // Runs `fn` on a fresh thread and reports whether it raised an
    // AssertionFailure. The catch must happen ON that thread: an exception
    // escaping a std::thread terminates the process rather than failing a test.
    bool assertionFiresOnFreshThread(void (*fn)()) {
        std::atomic<bool> fired{false};
        pauseTracking();
        std::thread t([&]() {
            try { fn(); }
            catch (const CroCOSTest::AssertionFailure&) { fired.store(true); }
            catch (...) {}
        });
        t.join();
        resumeTracking();
        return fired.load();
    }
}

// A thread that never declared which CPU it runs as must not receive an
// invented answer — every per-CPU structure would silently use it as an index.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_UnboundThreadCannotReadProcessorId, 5000) {
    ASSERT_TRUE(assertionFiresOnFreshThread([]() {
        (void)arch::getCurrentProcessorID();
    }));
}

// The invariant proper: two live threads may not hold one logical CPU. The
// harness binds this test's body thread to CPU 0, so a worker taking CPU 0 is
// exactly the overlap that used to go unnoticed.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_TwoLiveThreadsCannotShareOneCpu, 5000) {
    ASSERT_EQ(arch::ProcessorID{0}, arch::getCurrentProcessorID());

    ASSERT_TRUE(assertionFiresOnFreshThread([]() {
        arch::testing::ProcessorBinding cpu(0);
    }));
}

// Releasing the driver's CPU is what makes the stress tests legitimate: the
// driver is parked in join() and occupies no CPU, so a worker may take it.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_DetachedDriverYieldsItsCpuAndTakesItBack, 5000) {
    ASSERT_EQ(arch::ProcessorID{0}, arch::getCurrentProcessorID());

    std::atomic<bool> workerBound{false};
    {
        arch::testing::DetachedDriver parked;

        pauseTracking();
        std::thread worker([&]() {
            arch::testing::ProcessorBinding cpu(0);
            workerBound.store(arch::getCurrentProcessorID() == 0);
        });
        worker.join();
        resumeTracking();
    }

    ASSERT_TRUE(workerBound.load());
    // The guard restored the driver's own binding on the way out.
    ASSERT_EQ(arch::ProcessorID{0}, arch::getCurrentProcessorID());
}

// While detached the driver holds no CPU, so it must not be able to read one
// either — otherwise "detached" would be a comment rather than a state.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_DetachedDriverHoldsNoCpu, 5000) {
    arch::testing::DetachedDriver parked;
    EXPECT_ASSERT_FAILURE(arch::getCurrentProcessorID());
}

// A binding outside the configured processor count is the shape that produced
// the original SEGV: an ID the allocator has no LocalPool for.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_BindingBeyondProcessorCountIsRejected, 5000) {
    const auto count = arch::testing::getProcessorCount();
    EXPECT_ASSERT_FAILURE(
        arch::testing::ProcessorBinding(static_cast<arch::ProcessorID>(count)));
}

// Lowering the count under a live binding would turn a valid ID into an
// out-of-range one without anybody touching the binding.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_ProcessorCountCannotStrandALiveBinding, 5000) {
    const auto original = arch::testing::getProcessorCount();

    arch::testing::ProcessorBinding high(static_cast<arch::ProcessorID>(original - 1));
    EXPECT_ASSERT_FAILURE(arch::testing::setProcessorCount(original - 1));

    ASSERT_EQ(original, arch::testing::getProcessorCount());
}

// Nested bindings restore, so a helper that rebinds cannot strand its caller.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_NestedBindingRestoresThePrevious, 5000) {
    ASSERT_EQ(arch::ProcessorID{0}, arch::getCurrentProcessorID());
    {
        arch::testing::ProcessorBinding inner(3);
        ASSERT_EQ(arch::ProcessorID{3}, arch::getCurrentProcessorID());
    }
    ASSERT_EQ(arch::ProcessorID{0}, arch::getCurrentProcessorID());
}

// Distinct CPUs are fine concurrently — the guard must not reject the case the
// stress tests actually rely on.
TEST_WITH_TIMEOUT_NO_TRACKING(ArchMock_DistinctCpusMayRunConcurrently, 5000) {
    constexpr int workers = 4;
    std::atomic<int> ok{0};
    std::atomic<bool> release{false};

    {
        arch::testing::DetachedDriver parked;

        pauseTracking();
        std::vector<std::thread> threads;
        for (int t = 0; t < workers; t++) {
            threads.emplace_back([&, t]() {
                arch::testing::ProcessorBinding cpu(static_cast<arch::ProcessorID>(t));
                // Hold the binding while every other worker holds its own, so
                // the overlap is genuinely simultaneous rather than sequential.
                ok.fetch_add(arch::getCurrentProcessorID() == t ? 1 : 0);
                while (!release.load(std::memory_order_acquire)) {}
            });
        }
        while (ok.load() < workers) {}
        release.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
        resumeTracking();
    }

    ASSERT_EQ(workers, ok.load());
}
