//
// Concurrent stress tests for LibAlloc::SlabBookkeeper.
// Created by Spencer Martin on 5/27/26.
//
// Verifies SlabBookkeeper's single-owner / multi-freer contract under
// concurrent access. Built as a separate translation unit so the
// LibAllocTestRunner can stay single-threaded and fast, while
// LibAllocTestRunnerTSan picks these up under ThreadSanitizer to surface
// any weak-memory regressions on the ARMv8 dev machine (DEC-042 #4).
//
// Cases:
//   - MultiCpuFree_AllocatedCountReturnsToZero:
//       one owner allocates, K freers concurrently release; allocatedCount
//       must end at exactly zero across kWordCount equivalence classes.
//   - ObserverConsistency:
//       a polling observer sees allocatedCount strictly within
//       [0, maxAlloc] while the owner+freer thrash.
//   - TransitionBalance:
//       count becameFull and becameAvailable transitions across the run;
//       they must balance at quiescence.
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <liballoc/Slab.h>

#include <atomic>
#include <thread>
#include <vector>
#include <random>
#include <cstdint>

using namespace CroCOSTest;
using LibAlloc::SlabBookkeeper;
using Core::OccupancyTransition;

// ------------------------------------------------------------
// Helper: owner allocates all N slots, K freers shard them and release.
// ------------------------------------------------------------
template <size_t N>
static void runMultiCpuFreeRoundTrip(size_t freerThreads) {
    SlabBookkeeper<N> sb;
    sb.seedAllAvailable();

    // Owner phase: claim every slot, remember the indices.
    std::vector<int> claimed;
    claimed.reserve(N);
    OccupancyTransition t{};
    for (size_t i = 0; i < N; i++) {
        int slot = sb.allocSlot(t);
        ASSERT_GE(slot, 0);
        claimed.push_back(slot);
    }
    ASSERT_TRUE(sb.isFull());

    // Sharded multi-thread free phase.
    std::vector<std::thread> freers;
    freers.reserve(freerThreads);
    for (size_t f = 0; f < freerThreads; f++) {
        freers.emplace_back([&, f] {
            OccupancyTransition local{};
            for (size_t i = f; i < claimed.size(); i += freerThreads) {
                sb.freeSlot(static_cast<size_t>(claimed[i]), local);
            }
        });
    }
    for (auto& th : freers) th.join();

    ASSERT_EQ(size_t(0), sb.allocatedSlotCount());
    ASSERT_TRUE(sb.isEmpty());
}

TEST_WITH_TIMEOUT_NO_TRACKING(
    SlabBookkeeper_Concurrent_MultiCpuFree_AllocatedCountReturnsToZero_64, 5000) {
    runMultiCpuFreeRoundTrip<64>(4);
}
TEST_WITH_TIMEOUT_NO_TRACKING(
    SlabBookkeeper_Concurrent_MultiCpuFree_AllocatedCountReturnsToZero_137, 5000) {
    runMultiCpuFreeRoundTrip<137>(4);
}
TEST_WITH_TIMEOUT_NO_TRACKING(
    SlabBookkeeper_Concurrent_MultiCpuFree_AllocatedCountReturnsToZero_256, 5000) {
    runMultiCpuFreeRoundTrip<256>(4);
}

// ------------------------------------------------------------
// Observer-consistency: while the owner alternates alloc/free in tight
// loops and freer threads concurrently release, a polling observer must
// never see allocatedSlotCount outside [0, N].
// ------------------------------------------------------------
TEST_WITH_TIMEOUT_NO_TRACKING(SlabBookkeeper_Concurrent_ObserverConsistency, 5000) {
    constexpr size_t N = 137;
    SlabBookkeeper<N> sb;
    sb.seedAllAvailable();

    constexpr size_t kRounds = 4000;

    std::atomic<bool> stop{false};
    std::atomic<bool> observerSawOutOfRange{false};

    // Owner: cycles alloc/free in batches, handing freed indices off to
    // freer via a small SPSC ring.
    constexpr size_t kRingSize = 64;
    std::atomic<int> ring[kRingSize];
    for (auto& slot : ring) slot.store(-1, std::memory_order_relaxed);
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    std::thread owner([&] {
        OccupancyTransition local{};
        for (size_t r = 0; r < kRounds; r++) {
            int s = sb.allocSlot(local);
            if (s < 0) {
                std::this_thread::yield();
                continue;
            }
            // Wait until the freer drained the slot we're about to write.
            const size_t h = head.load(std::memory_order_relaxed);
            while ((h - tail.load(std::memory_order_acquire)) >= kRingSize) {
                std::this_thread::yield();
            }
            ring[h % kRingSize].store(s, std::memory_order_release);
            head.store(h + 1, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread freer([&] {
        OccupancyTransition local{};
        while (true) {
            const size_t t_ = tail.load(std::memory_order_relaxed);
            const size_t h_ = head.load(std::memory_order_acquire);
            if (t_ == h_) {
                if (stop.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            int s = ring[t_ % kRingSize].exchange(-1, std::memory_order_acquire);
            if (s < 0) continue;
            sb.freeSlot(static_cast<size_t>(s), local);
            tail.store(t_ + 1, std::memory_order_release);
        }
    });

    std::thread observer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const size_t cur = sb.allocatedSlotCount();
            if (cur > N) {
                observerSawOutOfRange.store(true, std::memory_order_relaxed);
                break;
            }
            // Touch the predicates so TSan sees the reads too.
            (void)sb.isFull();
            (void)sb.isEmpty();
        }
    });

    owner.join();
    freer.join();
    observer.join();

    ASSERT_FALSE(observerSawOutOfRange.load());

    // Drain any leftover allocations.
    while (sb.allocatedSlotCount() > 0) {
        size_t t_ = tail.load();
        size_t h_ = head.load();
        if (t_ == h_) break;
        int s = ring[t_ % kRingSize].exchange(-1);
        if (s >= 0) {
            OccupancyTransition local{};
            sb.freeSlot(static_cast<size_t>(s), local);
        }
        tail.store(t_ + 1);
    }
}

// ------------------------------------------------------------
// TransitionBalance: every becameFull eventually pairs with a
// becameAvailable. After quiescence the totals must match.
// ------------------------------------------------------------
TEST_WITH_TIMEOUT_NO_TRACKING(SlabBookkeeper_Concurrent_TransitionBalance, 5000) {
    constexpr size_t N = 64;
    SlabBookkeeper<N> sb;
    sb.seedAllAvailable();

    constexpr size_t kRounds = 2000;

    std::atomic<size_t> becameFullCount{0};
    std::atomic<size_t> becameAvailableCount{0};

    // SPSC hand-off ring just like above.
    constexpr size_t kRingSize = 64;
    std::atomic<int> ring[kRingSize];
    for (auto& slot : ring) slot.store(-1, std::memory_order_relaxed);
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
    std::atomic<bool> stop{false};

    std::thread owner([&] {
        OccupancyTransition local{};
        for (size_t r = 0; r < kRounds; r++) {
            int s = sb.allocSlot(local);
            if (s < 0) {
                std::this_thread::yield();
                continue;
            }
            if (local.becameFull()) {
                becameFullCount.fetch_add(1, std::memory_order_relaxed);
            }
            const size_t h = head.load(std::memory_order_relaxed);
            while ((h - tail.load(std::memory_order_acquire)) >= kRingSize) {
                std::this_thread::yield();
            }
            ring[h % kRingSize].store(s, std::memory_order_release);
            head.store(h + 1, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread freer([&] {
        OccupancyTransition local{};
        while (true) {
            const size_t t_ = tail.load(std::memory_order_relaxed);
            const size_t h_ = head.load(std::memory_order_acquire);
            if (t_ == h_) {
                if (stop.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            int s = ring[t_ % kRingSize].exchange(-1, std::memory_order_acquire);
            if (s < 0) continue;
            sb.freeSlot(static_cast<size_t>(s), local);
            if (local.becameAvailable()) {
                becameAvailableCount.fetch_add(1, std::memory_order_relaxed);
            }
            tail.store(t_ + 1, std::memory_order_release);
        }
    });

    owner.join();
    freer.join();

    // At quiescence: every Full→Partial transition pairs with a prior
    // Partial→Full transition. Note these aren't bookkeeping invariants
    // mid-flight (the count snapshot of allocatedCount is approximate
    // under concurrent fetch_add/fetch_sub interleavings); they must
    // balance only once the queue drains.
    ASSERT_EQ(becameFullCount.load(), becameAvailableCount.load());
}
