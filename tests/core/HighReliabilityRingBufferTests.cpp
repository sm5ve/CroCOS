//
// Unit tests for HighReliabilityRingBuffer
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/ds/HighReliabilityRingBuffer.h>

#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>

using namespace CroCOSTest;

// ============================================================
// Basic SPSC
// ============================================================

TEST(HRRBBasicWriteRead) {
    HighReliabilityRingBuffer<int> rb(8);

    ASSERT_TRUE(rb.empty());
    ASSERT_FALSE(rb.full());
    ASSERT_EQ(8u, rb.availableToWrite());
    ASSERT_EQ(0u, rb.availableToRead());

    bool ok = rb.bulkWrite(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i * 10);
    });
    ASSERT_TRUE(ok);
    ASSERT_EQ(3u, rb.availableToRead());

    int values[3] = {};
    size_t readCount = rb.bulkReadBestEffort(3, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_EQ(3u, readCount);
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);

    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(0u, rb.availableToRead());
}

TEST(HRRBSingleWriteRead) {
    HighReliabilityRingBuffer<int> rb(4);

    ASSERT_TRUE(rb.write(42));
    ASSERT_EQ(1u, rb.availableToRead());

    int out = 0;
    ASSERT_TRUE(rb.tryRead(out));
    ASSERT_EQ(42, out);
    ASSERT_TRUE(rb.empty());
}

TEST(HRRBFullAndEmpty) {
    HighReliabilityRingBuffer<int> rb(4);

    rb.bulkWrite(4, [](size_t i, int& slot) { slot = static_cast<int>(i); });
    ASSERT_TRUE(rb.full());
    ASSERT_EQ(0u, rb.availableToWrite());

    rb.bulkReadBestEffort(4, [](size_t, const int&) {});
    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(4u, rb.availableToWrite());
}

TEST(HRRBBulkReadAllOrNothing) {
    HighReliabilityRingBuffer<int> rb(8);

    rb.bulkWrite(3, [](size_t i, int& slot) { slot = static_cast<int>(i); });

    // Too few items — should fail without reading anything
    bool ok = rb.bulkRead(5, [](size_t, const int&) {});
    ASSERT_FALSE(ok);
    ASSERT_EQ(3u, rb.availableToRead());

    // Exact count — should succeed
    int vals[3] = {};
    ok = rb.bulkRead(3, [&vals](size_t i, const int& slot) { vals[i] = slot; });
    ASSERT_TRUE(ok);
    ASSERT_EQ(0, vals[0]);
    ASSERT_EQ(1, vals[1]);
    ASSERT_EQ(2, vals[2]);
    ASSERT_TRUE(rb.empty());
}

TEST(HRRBBulkWriteBestEffort) {
    HighReliabilityRingBuffer<int> rb(4);

    // Fill 3 of 4 slots
    size_t written = rb.bulkWriteBestEffort(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i);
    });
    ASSERT_EQ(3u, written);
    ASSERT_EQ(3u, rb.availableToRead());

    rb.bulkReadBestEffort(3, [](size_t, const int&) {});
    ASSERT_TRUE(rb.empty());
}

TEST(HRRBWrapAround) {
    HighReliabilityRingBuffer<int> rb(4);

    for (int round = 0; round < 4; round++) {
        rb.bulkWrite(4, [round](size_t i, int& slot) {
            slot = round * 100 + static_cast<int>(i);
        });
        int vals[4] = {};
        rb.bulkReadBestEffort(4, [&vals](size_t i, const int& slot) { vals[i] = slot; });
        for (int i = 0; i < 4; i++) {
            ASSERT_EQ(round * 100 + i, vals[i]);
        }
    }
}

// ============================================================
// MPSC — multiple producers, single consumer
// ============================================================

TEST(HRRBMPSCNoItemsLost) {
    constexpr size_t numProducers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;

    HighReliabilityRingBuffer<size_t> rb(totalItems);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.write(value)) {}
        }
    };

    std::vector<size_t> consumed;
    consumed.reserve(totalItems);

    auto consumer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (consumed.size() < totalItems) {
            rb.bulkReadBestEffort(16, [&](size_t, const size_t& slot) {
                consumed.push_back(slot);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t p = 0; p < numProducers; p++) threads.emplace_back(producer, p);
    threads.emplace_back(consumer);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    ASSERT_EQ(totalItems, consumed.size());
    std::sort(consumed.begin(), consumed.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(consumed.begin(), consumed.end(), p * 10000 + i));
        }
    }
}

// ============================================================
// SPMC — single producer, multiple consumers
// ============================================================

TEST(HRRBSPMCNoItemsLost) {
    constexpr size_t numConsumers = 4;
    constexpr size_t totalItems = 800;

    HighReliabilityRingBuffer<size_t> rb(totalItems);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    auto producer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < totalItems; i++) {
            while (!rb.write(i)) {}
        }
    };

    std::vector<std::vector<size_t>> perConsumer(numConsumers);
    auto consumer = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(8, [&](size_t, const size_t& slot) {
                perConsumer[id].push_back(slot);
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    threads.emplace_back(producer);
    for (size_t c = 0; c < numConsumers; c++) threads.emplace_back(consumer, c);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::vector<size_t> allConsumed;
    for (auto& v : perConsumer) allConsumed.insert(allConsumed.end(), v.begin(), v.end());
    ASSERT_EQ(totalItems, allConsumed.size());

    std::sort(allConsumed.begin(), allConsumed.end());
    for (size_t i = 0; i < totalItems; i++) {
        ASSERT_TRUE(std::binary_search(allConsumed.begin(), allConsumed.end(), i));
    }
}

// ============================================================
// MPMC — multiple producers and consumers
// ============================================================

TEST(HRRBMPMCNoItemsLost) {
    constexpr size_t numProducers = 4;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;

    HighReliabilityRingBuffer<size_t> rb(totalItems);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.write(value)) {}
        }
    };

    std::vector<std::vector<size_t>> perConsumer(numConsumers);
    auto consumer = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(8, [&](size_t, const size_t& slot) {
                perConsumer[id].push_back(slot);
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t p = 0; p < numProducers; p++) threads.emplace_back(producer, p);
    for (size_t c = 0; c < numConsumers; c++) threads.emplace_back(consumer, c);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::vector<size_t> allConsumed;
    for (auto& v : perConsumer) allConsumed.insert(allConsumed.end(), v.begin(), v.end());
    ASSERT_EQ(totalItems, allConsumed.size());

    std::sort(allConsumed.begin(), allConsumed.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(allConsumed.begin(), allConsumed.end(), p * 10000 + i));
        }
    }
}

// ============================================================
// ScanOnComplete spin correctness — regression test for the race
// that motivated this wrapper. Uses ScanOnComplete=true, which
// is the variant where writtenHead can lag behind logicalCount.
// ============================================================

TEST(HRRBScanOnCompleteNoItemsLost) {
    constexpr size_t numProducers = 8;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;

    HighReliabilityRingBuffer<size_t, true, true> rb(totalItems);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.write(value)) {}
        }
    };

    std::vector<std::vector<size_t>> perConsumer(numConsumers);
    auto consumer = [&](size_t id) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(8, [&](size_t, const size_t& slot) {
                perConsumer[id].push_back(slot);
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (size_t p = 0; p < numProducers; p++) threads.emplace_back(producer, p);
    for (size_t c = 0; c < numConsumers; c++) threads.emplace_back(consumer, c);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::vector<size_t> allConsumed;
    for (auto& v : perConsumer) allConsumed.insert(allConsumed.end(), v.begin(), v.end());
    ASSERT_EQ(totalItems, allConsumed.size());

    std::sort(allConsumed.begin(), allConsumed.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(allConsumed.begin(), allConsumed.end(), p * 10000 + i));
        }
    }
}

// ============================================================
// Comparison: read failure rates under heavy write contention
//
// Runs the same MPMC workload on:
//   (A) SimpleMPMCRingBuffer<size_t, true, false>  — no scan
//   (B) SimpleMPMCRingBuffer<size_t, true, true>   — scan, no HRRB
//   (C) HighReliabilityRingBuffer<size_t, true, true> — scan + HRRB
//
// Asserts that (C) has zero read failures.
// ============================================================

namespace {

struct ComparisonResult {
    size_t totalConsumed;
    size_t totalItems;
};

template<typename RB>
ComparisonResult runContendedMPMC(RB& rb, size_t numProducers,
                                   size_t numConsumers, size_t itemsPerProducer) {
    const size_t totalItems = numProducers * itemsPerProducer;
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.write(value)) {}
        }
    };

    auto consumer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(8, [&](size_t, const size_t&) {
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    std::vector<std::thread> threads;
    for (size_t p = 0; p < numProducers; p++) threads.emplace_back(producer, p);
    for (size_t c = 0; c < numConsumers; c++) threads.emplace_back(consumer);

    start.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    return {totalConsumed.load(), totalItems};
}

} // namespace

TEST(HRRBComparisonZeroFailures) {
    constexpr size_t numProducers = 8;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 500;
    const size_t totalItems = numProducers * itemsPerProducer;

    pauseTracking();
    HighReliabilityRingBuffer<size_t, true, true> hrrb(totalItems);
    auto result = runContendedMPMC(hrrb, numProducers, numConsumers, itemsPerProducer);
    resumeTracking();

    ASSERT_EQ(result.totalItems, result.totalConsumed);
}
