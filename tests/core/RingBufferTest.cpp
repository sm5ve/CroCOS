//
// Unit tests for SimpleMPMCRingBuffer and MPMCRingBuffer
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/atomic/RingBuffer.h>

#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>

using namespace CroCOSTest;

// ============================================================
// SimpleMPMCRingBuffer - Single Producer, Single Consumer
// ============================================================

TEST(SimpleRingBufferSPSCBasicWriteRead) {
    SimpleMPMCRingBuffer<int> rb(8);

    ASSERT_TRUE(rb.empty());
    ASSERT_FALSE(rb.full());
    ASSERT_EQ(8u, rb.availableToWrite());
    ASSERT_EQ(0u, rb.availableToRead());

    // Write 3 items
    bool ok = rb.tryBulkWrite(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i * 10);
    });
    ASSERT_TRUE(ok);
    ASSERT_EQ(5u, rb.availableToWrite());
    ASSERT_EQ(3u, rb.availableToRead());

    // Read them back
    int values[3] = {};
    ok = rb.tryBulkRead(3, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_TRUE(ok);
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);

    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(8u, rb.availableToWrite());
}

TEST(SimpleRingBufferSPSCFillAndDrain) {
    SimpleMPMCRingBuffer<int> rb(4);

    // Fill completely
    bool ok = rb.tryBulkWrite(4, [](size_t i, int& slot) {
        slot = static_cast<int>(i);
    });
    ASSERT_TRUE(ok);
    ASSERT_TRUE(rb.full());
    ASSERT_EQ(0u, rb.availableToWrite());

    // Cannot write more
    ok = rb.tryBulkWrite(1, [](size_t, int& slot) { slot = 99; });
    ASSERT_FALSE(ok);

    // Drain completely
    int values[4] = {};
    ok = rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_TRUE(ok);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(i, values[i]);
    }
    ASSERT_TRUE(rb.empty());
}

TEST(SimpleRingBufferSPSCWraparound) {
    SimpleMPMCRingBuffer<int> rb(4);

    // Fill, drain, then fill again to force wraparound
    for (int round = 0; round < 5; round++) {
        bool ok = rb.tryBulkWrite(4, [round](size_t i, int& slot) {
            slot = round * 100 + static_cast<int>(i);
        });
        ASSERT_TRUE(ok);

        int values[4] = {};
        ok = rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
            values[i] = slot;
        });
        ASSERT_TRUE(ok);
        for (int i = 0; i < 4; i++) {
            ASSERT_EQ(round * 100 + i, values[i]);
        }
    }
}

TEST(SimpleRingBufferSPSCPartialWraparound) {
    SimpleMPMCRingBuffer<int> rb(8);

    // Write 5, read 5, write 5 again (wraps around slot indices)
    rb.tryBulkWrite(5, [](size_t i, int& slot) {
        slot = static_cast<int>(i);
    });
    int values[5] = {};
    rb.tryBulkRead(5, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    for (int i = 0; i < 5; i++) ASSERT_EQ(i, values[i]);

    // This write wraps: slots 5,6,7,0,1
    rb.tryBulkWrite(5, [](size_t i, int& slot) {
        slot = static_cast<int>(i + 100);
    });
    rb.tryBulkRead(5, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    for (int i = 0; i < 5; i++) ASSERT_EQ(i + 100, values[i]);
}

// ============================================================
// SimpleMPMCRingBuffer - try* failure cases
// ============================================================

TEST(SimpleRingBufferTryWriteFailsWhenFull) {
    SimpleMPMCRingBuffer<int> rb(2);

    ASSERT_TRUE(rb.tryBulkWrite(2, [](size_t i, int& slot) { slot = static_cast<int>(i); }));
    ASSERT_FALSE(rb.tryBulkWrite(1, [](size_t, int& slot) { slot = 0; }));
}

TEST(SimpleRingBufferTryReadFailsWhenEmpty) {
    SimpleMPMCRingBuffer<int> rb(4);

    ASSERT_FALSE(rb.tryBulkRead(1, [](size_t, const int&) {}));
}

TEST(SimpleRingBufferTryReadFailsWhenInsufficient) {
    SimpleMPMCRingBuffer<int> rb(8);

    rb.tryBulkWrite(2, [](size_t i, int& slot) { slot = static_cast<int>(i); });
    ASSERT_FALSE(rb.tryBulkRead(3, [](size_t, const int&) {}));
    // But 2 should succeed
    ASSERT_TRUE(rb.tryBulkRead(2, [](size_t, const int&) {}));
}

// ============================================================
// SimpleMPMCRingBuffer - Best-effort variants
// ============================================================

TEST(SimpleRingBufferBestEffortWritePartial) {
    SimpleMPMCRingBuffer<int> rb(4);

    // Write 3 of 4 slots
    rb.tryBulkWrite(3, [](size_t i, int& slot) { slot = static_cast<int>(i); });

    // Try best-effort write of 4, should only get 1
    size_t written = rb.bulkWriteBestEffort(4, [](size_t i, int& slot) {
        slot = static_cast<int>(i + 100);
    });
    ASSERT_EQ(1u, written);
    ASSERT_TRUE(rb.full());
}

TEST(SimpleRingBufferBestEffortWriteZero) {
    SimpleMPMCRingBuffer<int> rb(2);

    rb.tryBulkWrite(2, [](size_t i, int& slot) { slot = static_cast<int>(i); });

    size_t written = rb.bulkWriteBestEffort(5, [](size_t, int& slot) { slot = 0; });
    ASSERT_EQ(0u, written);
}

TEST(SimpleRingBufferBestEffortReadPartial) {
    SimpleMPMCRingBuffer<int> rb(8);

    rb.tryBulkWrite(3, [](size_t i, int& slot) { slot = static_cast<int>(i * 10); });

    int values[8] = {};
    size_t read = rb.bulkReadBestEffort(5, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_EQ(3u, read);
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);
}

TEST(SimpleRingBufferBestEffortReadZero) {
    SimpleMPMCRingBuffer<int> rb(4);

    size_t read = rb.bulkReadBestEffort(2, [](size_t, const int&) {});
    ASSERT_EQ(0u, read);
}

// ============================================================
// SimpleMPMCRingBuffer - Non-owning variant
// ============================================================

TEST(SimpleRingBufferNonOwning) {
    int externalBuffer[8];
    SimpleMPMCRingBuffer<int, false> rb(externalBuffer, 8);

    ASSERT_TRUE(rb.tryBulkWrite(4, [](size_t i, int& slot) {
        slot = static_cast<int>(i + 1);
    }));

    // Verify data is written to the external buffer
    ASSERT_EQ(1, externalBuffer[0]);
    ASSERT_EQ(2, externalBuffer[1]);
    ASSERT_EQ(3, externalBuffer[2]);
    ASSERT_EQ(4, externalBuffer[3]);

    int values[4] = {};
    ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
        values[i] = slot;
    }));
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(i + 1, values[i]);
    }
}

// ============================================================
// SimpleMPMCRingBuffer - Multi-threaded: MPSC
// ============================================================

TEST(SimpleRingBufferMPSC) {
    // Buffer must be large enough to hold all items since SimpleMPMCRingBuffer
    // has no read-completion protection (slots may be overwritten while readers
    // are still in callbacks if the buffer wraps).
    constexpr size_t numProducers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalExpected = numProducers * itemsPerProducer;
    constexpr size_t bufSize = 1024;

    SimpleMPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    // Single consumer: collect all values
    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(16, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    // Verify all values present
    ASSERT_EQ(totalExpected, collected.size());
    std::sort(collected.begin(), collected.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t expected = p * 10000 + i;
            ASSERT_TRUE(std::binary_search(collected.begin(), collected.end(), expected));
        }
    }
}

// ============================================================
// SimpleMPMCRingBuffer - Multi-threaded: SPMC
// ============================================================

TEST(SimpleRingBufferSPMC) {
    constexpr size_t numConsumers = 4;
    constexpr size_t totalItems = 800;
    constexpr size_t bufSize = 1024;

    SimpleMPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    // Pre-allocated storage to avoid vector growth inside thread callbacks
    size_t consumedValues[totalItems];
    std::atomic<size_t> consumedIndex{0};

    auto consumer = [&](size_t) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                size_t idx = consumedIndex.fetch_add(1, std::memory_order_relaxed);
                consumedValues[idx] = slot;
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < numConsumers; c++) {
        consumers.emplace_back(consumer, c);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    // Single producer: write all values
    size_t produced = 0;
    while (produced < totalItems) {
        size_t val = produced;
        if (rb.tryBulkWrite(1, [val](size_t, size_t& slot) { slot = val; })) {
            produced++;
        }
    }

    pauseTracking();
    for (auto& t : consumers) t.join();
    resumeTracking();

    // Verify all values present
    size_t count = consumedIndex.load();
    ASSERT_EQ(totalItems, count);
    std::sort(consumedValues, consumedValues + count);
    for (size_t i = 0; i < totalItems; i++) {
        ASSERT_EQ(i, consumedValues[i]);
    }
}

// ============================================================
// SimpleMPMCRingBuffer - Multi-threaded: MPMC
// ============================================================

TEST(SimpleRingBufferMPMC) {
    constexpr size_t numProducers = 4;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;
    // Buffer must hold all items to avoid wrapping (SimpleMPMCRingBuffer has
    // no read-completion protection).
    constexpr size_t bufSize = totalItems;

    SimpleMPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    std::atomic<size_t> totalConsumed{0};
    std::vector<std::vector<size_t>> perConsumer(numConsumers);

    auto consumer = [&](size_t consumerId) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                perConsumer[consumerId].push_back(slot);
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
// SimpleMPMCRingBuffer - Write completion ordering (MPSC)
// ============================================================

TEST(SimpleRingBufferWriteCompletionOrdering) {
    // Verifies that writtenHead advances in order even when
    // producers complete out-of-order.
    constexpr size_t numProducers = 8;
    constexpr size_t batchSize = 4;
    constexpr size_t batchesPerProducer = 10;
    constexpr size_t totalExpected = numProducers * batchesPerProducer * batchSize;
    constexpr size_t bufSize = totalExpected;

    SimpleMPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t batch = 0; batch < batchesPerProducer; batch++) {
            size_t base = producerId * 1000 + batch * batchSize;
            while (!rb.tryBulkWrite(batchSize, [base](size_t i, size_t& slot) {
                slot = base + i;
            })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    // Consumer reads and verifies all items arrive
    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(batchSize, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    ASSERT_EQ(totalExpected, collected.size());
}

// ============================================================
// MPMCRingBuffer - Single Producer, Single Consumer
// ============================================================

TEST(MPMCRingBufferSPSCBasicWriteRead) {
    MPMCRingBuffer<int> rb(8);

    ASSERT_TRUE(rb.empty());
    ASSERT_FALSE(rb.full());

    bool ok = rb.tryBulkWrite(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i * 10);
    });
    ASSERT_TRUE(ok);
    ASSERT_EQ(3u, rb.availableToRead());

    int values[3] = {};
    ok = rb.tryBulkRead(3, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_TRUE(ok);
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);
    ASSERT_TRUE(rb.empty());
}

TEST(MPMCRingBufferSPSCFillAndDrain) {
    MPMCRingBuffer<int> rb(4);

    ASSERT_TRUE(rb.tryBulkWrite(4, [](size_t i, int& slot) { slot = static_cast<int>(i); }));
    ASSERT_TRUE(rb.full());
    ASSERT_FALSE(rb.tryBulkWrite(1, [](size_t, int& slot) { slot = 0; }));

    int values[4] = {};
    ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) { values[i] = slot; }));
    for (int i = 0; i < 4; i++) ASSERT_EQ(i, values[i]);
    ASSERT_TRUE(rb.empty());
}

TEST(MPMCRingBufferSPSCWraparound) {
    MPMCRingBuffer<int> rb(4);

    for (int round = 0; round < 5; round++) {
        ASSERT_TRUE(rb.tryBulkWrite(4, [round](size_t i, int& slot) {
            slot = round * 100 + static_cast<int>(i);
        }));

        int values[4] = {};
        ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
            values[i] = slot;
        }));
        for (int i = 0; i < 4; i++) ASSERT_EQ(round * 100 + i, values[i]);
    }
}

// ============================================================
// MPMCRingBuffer - try* failure cases
// ============================================================

TEST(MPMCRingBufferTryWriteFailsWhenFull) {
    MPMCRingBuffer<int> rb(2);

    ASSERT_TRUE(rb.tryBulkWrite(2, [](size_t i, int& slot) { slot = static_cast<int>(i); }));
    ASSERT_FALSE(rb.tryBulkWrite(1, [](size_t, int& slot) { slot = 0; }));
}

TEST(MPMCRingBufferTryReadFailsWhenEmpty) {
    MPMCRingBuffer<int> rb(4);
    ASSERT_FALSE(rb.tryBulkRead(1, [](size_t, const int&) {}));
}

// ============================================================
// MPMCRingBuffer - Best-effort variants
// ============================================================

TEST(MPMCRingBufferBestEffortWritePartial) {
    MPMCRingBuffer<int> rb(4);

    rb.tryBulkWrite(3, [](size_t i, int& slot) { slot = static_cast<int>(i); });

    size_t written = rb.bulkWriteBestEffort(4, [](size_t i, int& slot) {
        slot = static_cast<int>(i + 100);
    });
    ASSERT_EQ(1u, written);
    ASSERT_TRUE(rb.full());
}

TEST(MPMCRingBufferBestEffortReadPartial) {
    MPMCRingBuffer<int> rb(8);

    rb.tryBulkWrite(3, [](size_t i, int& slot) { slot = static_cast<int>(i * 10); });

    int values[8] = {};
    size_t read = rb.bulkReadBestEffort(5, [&values](size_t i, const int& slot) {
        values[i] = slot;
    });
    ASSERT_EQ(3u, read);
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);
}

// ============================================================
// MPMCRingBuffer - Non-owning variant
// ============================================================

TEST(MPMCRingBufferNonOwning) {
    int externalBuffer[8];
    MPMCRingBuffer<int, false> rb(externalBuffer, 8);

    ASSERT_TRUE(rb.tryBulkWrite(4, [](size_t i, int& slot) {
        slot = static_cast<int>(i + 1);
    }));

    ASSERT_EQ(1, externalBuffer[0]);
    ASSERT_EQ(2, externalBuffer[1]);
    ASSERT_EQ(3, externalBuffer[2]);
    ASSERT_EQ(4, externalBuffer[3]);

    int values[4] = {};
    ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
        values[i] = slot;
    }));
    for (int i = 0; i < 4; i++) ASSERT_EQ(i + 1, values[i]);
}

// ============================================================
// MPMCRingBuffer - Multi-threaded: MPSC
// ============================================================

TEST(MPMCRingBufferMPSC) {
    constexpr size_t bufSize = 1024;
    constexpr size_t numProducers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalExpected = numProducers * itemsPerProducer;

    MPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(16, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    ASSERT_EQ(totalExpected, collected.size());
    std::sort(collected.begin(), collected.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(collected.begin(), collected.end(), p * 10000 + i));
        }
    }
}

// ============================================================
// MPMCRingBuffer - Multi-threaded: SPMC
// ============================================================

TEST(MPMCRingBufferSPMC) {
    constexpr size_t bufSize = 1024;
    constexpr size_t numConsumers = 4;
    constexpr size_t totalItems = 800;

    MPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    size_t consumedValues[totalItems];
    std::atomic<size_t> consumedIndex{0};

    auto consumer = [&](size_t) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                size_t idx = consumedIndex.fetch_add(1, std::memory_order_relaxed);
                consumedValues[idx] = slot;
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < numConsumers; c++) {
        consumers.emplace_back(consumer, c);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    size_t produced = 0;
    while (produced < totalItems) {
        if (rb.tryBulkWrite(1, [produced](size_t, size_t& slot) { slot = produced; })) {
            produced++;
        }
    }

    pauseTracking();
    for (auto& t : consumers) t.join();
    resumeTracking();

    size_t count = consumedIndex.load();
    ASSERT_EQ(totalItems, count);
    std::sort(consumedValues, consumedValues + count);
    for (size_t i = 0; i < totalItems; i++) ASSERT_EQ(i, consumedValues[i]);
}

// ============================================================
// MPMCRingBuffer - Multi-threaded: MPMC
// ============================================================

TEST(MPMCRingBufferMPMC) {
    constexpr size_t bufSize = 512;
    constexpr size_t numProducers = 4;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;

    MPMCRingBuffer<size_t> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    std::atomic<size_t> totalConsumed{0};
    std::vector<std::vector<size_t>> perConsumer(numConsumers);

    auto consumer = [&](size_t consumerId) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                perConsumer[consumerId].push_back(slot);
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
// MPMCRingBuffer - bulkWrite spin-wait behavior
// ============================================================

TEST(MPMCRingBufferBulkWriteSpinWaits) {
    // Verify that bulkWrite (optimistic, checks readingHead) succeeds
    // after a consumer drains the buffer.
    constexpr size_t bufSize = 8;

    MPMCRingBuffer<size_t> rb(bufSize);

    // Fill buffer
    ASSERT_TRUE(rb.tryBulkWrite(bufSize, [](size_t i, size_t& slot) { slot = i; }));
    ASSERT_TRUE(rb.full());

    // Consumer drains the buffer
    pauseTracking();
    std::thread consumer([&] {
        rb.tryBulkRead(bufSize, [](size_t, const size_t&) {});
    });
    consumer.join();
    resumeTracking();

    // Buffer is empty, bulkWrite should succeed
    ASSERT_TRUE(rb.bulkWrite(bufSize, [](size_t i, size_t& slot) { slot = i + 100; }));

    size_t values[8] = {};
    ASSERT_TRUE(rb.tryBulkRead(bufSize, [&values](size_t i, const size_t& slot) {
        values[i] = slot;
    }));
    for (size_t i = 0; i < bufSize; i++) ASSERT_EQ(i + 100, values[i]);
}

// ============================================================
// SimpleMPMCRingBuffer<T, true, true> - ScanOnComplete variant
// ============================================================

TEST(SimpleRingBufferScanSPSCBasic) {
    SimpleMPMCRingBuffer<int, true, true> rb(8);

    ASSERT_TRUE(rb.tryBulkWrite(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i * 10);
    }));
    ASSERT_EQ(3u, rb.availableToRead());

    int values[3] = {};
    ASSERT_TRUE(rb.tryBulkRead(3, [&values](size_t i, const int& slot) {
        values[i] = slot;
    }));
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);
    ASSERT_TRUE(rb.empty());
}

TEST(SimpleRingBufferScanSPSCWraparound) {
    SimpleMPMCRingBuffer<int, true, true> rb(4);

    for (int round = 0; round < 5; round++) {
        ASSERT_TRUE(rb.tryBulkWrite(4, [round](size_t i, int& slot) {
            slot = round * 100 + static_cast<int>(i);
        }));
        int values[4] = {};
        ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
            values[i] = slot;
        }));
        for (int i = 0; i < 4; i++) ASSERT_EQ(round * 100 + i, values[i]);
    }
}

TEST(SimpleRingBufferScanMPSC) {
    constexpr size_t numProducers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalExpected = numProducers * itemsPerProducer;
    constexpr size_t bufSize = 1024;

    SimpleMPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(16, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    ASSERT_EQ(totalExpected, collected.size());
    std::sort(collected.begin(), collected.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(collected.begin(), collected.end(), p * 10000 + i));
        }
    }
}

TEST(SimpleRingBufferScanMPMC) {
    constexpr size_t numProducers = 4;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;
    constexpr size_t bufSize = totalItems;

    SimpleMPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    std::atomic<size_t> totalConsumed{0};
    std::vector<std::vector<size_t>> perConsumer(numConsumers);

    auto consumer = [&](size_t consumerId) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                perConsumer[consumerId].push_back(slot);
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

// Exercises out-of-order batch completion with scan-ahead: 8 producers
// writing batches of 4 should trigger the scan path to pick up
// subsequently completed batches in a single head advancement.
TEST(SimpleRingBufferScanWriteCompletionOrdering) {
    constexpr size_t numProducers = 8;
    constexpr size_t batchSize = 4;
    constexpr size_t batchesPerProducer = 10;
    constexpr size_t totalExpected = numProducers * batchesPerProducer * batchSize;
    constexpr size_t bufSize = totalExpected;

    SimpleMPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t batch = 0; batch < batchesPerProducer; batch++) {
            size_t base = producerId * 1000 + batch * batchSize;
            while (!rb.tryBulkWrite(batchSize, [base](size_t i, size_t& slot) {
                slot = base + i;
            })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(batchSize, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    ASSERT_EQ(totalExpected, collected.size());
}

// ============================================================
// MPMCRingBuffer<T, true, true> - ScanOnComplete variant
// ============================================================

TEST(MPMCRingBufferScanSPSCBasic) {
    MPMCRingBuffer<int, true, true> rb(8);

    ASSERT_TRUE(rb.tryBulkWrite(3, [](size_t i, int& slot) {
        slot = static_cast<int>(i * 10);
    }));

    int values[3] = {};
    ASSERT_TRUE(rb.tryBulkRead(3, [&values](size_t i, const int& slot) {
        values[i] = slot;
    }));
    ASSERT_EQ(0, values[0]);
    ASSERT_EQ(10, values[1]);
    ASSERT_EQ(20, values[2]);
    ASSERT_TRUE(rb.empty());
}

TEST(MPMCRingBufferScanSPSCWraparound) {
    MPMCRingBuffer<int, true, true> rb(4);

    for (int round = 0; round < 5; round++) {
        ASSERT_TRUE(rb.tryBulkWrite(4, [round](size_t i, int& slot) {
            slot = round * 100 + static_cast<int>(i);
        }));
        int values[4] = {};
        ASSERT_TRUE(rb.tryBulkRead(4, [&values](size_t i, const int& slot) {
            values[i] = slot;
        }));
        for (int i = 0; i < 4; i++) ASSERT_EQ(round * 100 + i, values[i]);
    }
}

TEST(MPMCRingBufferScanMPSC) {
    constexpr size_t bufSize = 1024;
    constexpr size_t numProducers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalExpected = numProducers * itemsPerProducer;

    MPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    pauseTracking();
    std::vector<std::thread> producers;
    for (size_t p = 0; p < numProducers; p++) {
        producers.emplace_back(producer, p);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    std::vector<size_t> collected;
    while (collected.size() < totalExpected) {
        rb.bulkReadBestEffort(16, [&collected](size_t, const size_t& slot) {
            collected.push_back(slot);
        });
    }

    pauseTracking();
    for (auto& t : producers) t.join();
    resumeTracking();

    ASSERT_EQ(totalExpected, collected.size());
    std::sort(collected.begin(), collected.end());
    for (size_t p = 0; p < numProducers; p++) {
        for (size_t i = 0; i < itemsPerProducer; i++) {
            ASSERT_TRUE(std::binary_search(collected.begin(), collected.end(), p * 10000 + i));
        }
    }
}

TEST(MPMCRingBufferScanMPMC) {
    constexpr size_t bufSize = 512;
    constexpr size_t numProducers = 4;
    constexpr size_t numConsumers = 4;
    constexpr size_t itemsPerProducer = 200;
    constexpr size_t totalItems = numProducers * itemsPerProducer;

    MPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};

    auto producer = [&](size_t producerId) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < itemsPerProducer; i++) {
            size_t value = producerId * 10000 + i;
            while (!rb.tryBulkWrite(1, [value](size_t, size_t& slot) { slot = value; })) {}
        }
    };

    std::atomic<size_t> totalConsumed{0};
    std::vector<std::vector<size_t>> perConsumer(numConsumers);

    auto consumer = [&](size_t consumerId) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                perConsumer[consumerId].push_back(slot);
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

// Exercises the read-completion scan path: 4 consumers with scan-ahead
// on the readHead, plus the write-completion scan path on the write side.
TEST(MPMCRingBufferScanSPMC) {
    constexpr size_t bufSize = 1024;
    constexpr size_t numConsumers = 4;
    constexpr size_t totalItems = 800;

    MPMCRingBuffer<size_t, true, true> rb(bufSize);
    std::atomic<bool> start{false};
    std::atomic<size_t> totalConsumed{0};

    size_t consumedValues[totalItems];
    std::atomic<size_t> consumedIndex{0};

    auto consumer = [&](size_t) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalConsumed.load(std::memory_order_acquire) < totalItems) {
            rb.bulkReadBestEffort(4, [&](size_t, const size_t& slot) {
                size_t idx = consumedIndex.fetch_add(1, std::memory_order_relaxed);
                consumedValues[idx] = slot;
                totalConsumed.fetch_add(1, std::memory_order_relaxed);
            });
        }
    };

    pauseTracking();
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < numConsumers; c++) {
        consumers.emplace_back(consumer, c);
    }
    resumeTracking();

    start.store(true, std::memory_order_release);

    size_t produced = 0;
    while (produced < totalItems) {
        if (rb.tryBulkWrite(1, [produced](size_t, size_t& slot) { slot = produced; })) {
            produced++;
        }
    }

    pauseTracking();
    for (auto& t : consumers) t.join();
    resumeTracking();

    size_t count = consumedIndex.load();
    ASSERT_EQ(totalItems, count);
    std::sort(consumedValues, consumedValues + count);
    for (size_t i = 0; i < totalItems; i++) ASSERT_EQ(i, consumedValues[i]);
}

// ============================================================
// MPMCRingBuffer - Bulk write with batch sizes
// ============================================================

TEST(MPMCRingBufferBulkBatchWriteRead) {
    MPMCRingBuffer<int> rb(16);

    // Write in batches of 4
    for (int batch = 0; batch < 4; batch++) {
        ASSERT_TRUE(rb.tryBulkWrite(4, [batch](size_t i, int& slot) {
            slot = batch * 100 + static_cast<int>(i);
        }));
    }

    // Read back in batches of 8
    for (int batch = 0; batch < 2; batch++) {
        int values[8] = {};
        ASSERT_TRUE(rb.tryBulkRead(8, [&values](size_t i, const int& slot) {
            values[i] = slot;
        }));
        for (int i = 0; i < 8; i++) {
            int srcBatch = (batch * 8 + i) / 4;
            int srcIdx = (batch * 8 + i) % 4;
            ASSERT_EQ(srcBatch * 100 + srcIdx, values[i]);
        }
    }
    ASSERT_TRUE(rb.empty());
}