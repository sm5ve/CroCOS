//
// Unit tests for AtomicBitPool
// Created by Spencer Martin on 4/7/26.
//
// Coverage:
//   - requiredBufferSize returns positive for all supported level counts
//   - entryStride clamping: small stride still yields valid buffer size
//   - Zero-initialized invariant holds after construction
//   - add: result codes (AddedToEmpty, AddedToNonempty, AlreadyPresent)
//   - add: fill pool, then verify re-add returns AddedToEmpty after emptying
//   - remove: result codes (NotPresent, RemovedAndMadeEmpty, RemovedAndStayedNonempty)
//   - remove: add/remove all in forward and reverse order, verify clean state
//   - After every operation: checkInvariants() passes (counts == L0 popcount,
//     hint bitmaps match nonzero counts)
//   - Edge cases: capacity 1, 64, 65
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/atomic/AtomicBitPool.h>

#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>

// ============================================================
// Convenience type aliases
// ============================================================

using AR = AtomicBitPool::AddResult;
using RR = AtomicBitPool::RemoveResult;
using GR = AtomicBitPool::GetResult;

// ============================================================
// PoolFixture: owns aligned buffer + pool lifetime
// ============================================================

struct PoolFixture {
    std::vector<uint8_t> buffer;
    AtomicBitPool*       pool = nullptr;

    explicit PoolFixture(size_t capacity, size_t entryStride = 64) {
        size_t sz = AtomicBitPool::requiredBufferSize(capacity, entryStride);
        buffer.resize(sz);
        pool = new AtomicBitPool(capacity, buffer.data(), entryStride);
    }

    ~PoolFixture() { delete pool; }
};

// Wrappers that assert the expected result and verify structural invariants.
static void checkedAdd(PoolFixture& f, size_t i, AR expected) {
    ASSERT_EQ(f.pool->add(i), expected);
    ASSERT_TRUE(f.pool->checkInvariants());
}

static void checkedRemove(PoolFixture& f, size_t i, RR expected) {
    ASSERT_EQ(f.pool->remove(i), expected);
    ASSERT_TRUE(f.pool->checkInvariants());
}

// Returns the claimed index; asserts Success and invariants.
static size_t checkedGetAnySuccess(PoolFixture& f, size_t threadId) {
    size_t idx = SIZE_MAX;
    ASSERT_EQ(f.pool->getAny(threadId, idx), GR::Success);
    ASSERT_TRUE(f.pool->checkInvariants());
    return idx;
}

static void checkedGetAny(PoolFixture& f, size_t threadId, GR expected) {
    size_t idx = SIZE_MAX;
    ASSERT_EQ(f.pool->getAny(threadId, idx), expected);
    ASSERT_TRUE(f.pool->checkInvariants());
}

// ============================================================
// requiredBufferSize: positive for all three level counts
//   small  (64)   → levelCount == 1
//   medium (4096) → levelCount == 2
//   large  (8192) → levelCount == 3, l1BitmapLog2Width == 1 < 6
// ============================================================

TEST(AtomicBitPool_BufferSizePositive_Small) {
    ASSERT_GT(AtomicBitPool::requiredBufferSize(64), size_t(0));
}

TEST(AtomicBitPool_BufferSizePositive_Medium) {
    ASSERT_GT(AtomicBitPool::requiredBufferSize(4096), size_t(0));
}

TEST(AtomicBitPool_BufferSizePositive_Large) {
    ASSERT_GT(AtomicBitPool::requiredBufferSize(8192), size_t(0));
}

// requiredBufferSize clamps entryStride < sizeof(Entry) and still returns > 0
TEST(AtomicBitPool_SmallStrideClamped) {
    size_t sz = AtomicBitPool::requiredBufferSize(64, 4);
    ASSERT_GT(sz, size_t(0));
    // Construction must not crash
    std::vector<uint8_t> buf(sz);
    AtomicBitPool pool(64, buf.data(), 4);
    (void)pool;
}

// ============================================================
// Zero after construction: checkInvariants passes on fresh pool,
// and the first add returns AddedToEmpty (confirming zero counts).
// ============================================================

static void testZeroAfterConstruction(size_t capacity) {
    PoolFixture f(capacity);
    ASSERT_TRUE(f.pool->checkInvariants());
    // A fresh pool is empty, so the first add must return AddedToEmpty.
    ASSERT_EQ(f.pool->add(0), AR::AddedToEmpty);
    ASSERT_TRUE(f.pool->checkInvariants());
}

TEST(AtomicBitPool_ZeroAfterConstruction_Small)  { testZeroAfterConstruction(64); }
TEST(AtomicBitPool_ZeroAfterConstruction_Medium) { testZeroAfterConstruction(4096); }
TEST(AtomicBitPool_ZeroAfterConstruction_Large)  { testZeroAfterConstruction(8192); }

// ============================================================
// add: result code correctness
// ============================================================

static void testAddResultCodes(size_t capacity) {
    PoolFixture f(capacity);

    // First add to an empty pool → AddedToEmpty
    checkedAdd(f, 0, AR::AddedToEmpty);
    // Add a distinct index to a non-empty pool → AddedToNonempty
    checkedAdd(f, 1, AR::AddedToNonempty);
    // Re-add an already-present index → AlreadyPresent
    checkedAdd(f, 0, AR::AlreadyPresent);
    // Add the last valid index → AddedToNonempty (pool already has elements)
    checkedAdd(f, capacity - 1, AR::AddedToNonempty);
    // Re-add the last valid index → AlreadyPresent
    checkedAdd(f, capacity - 1, AR::AlreadyPresent);
}

TEST(AtomicBitPool_AddResultCodes_Small)  { testAddResultCodes(64); }
TEST(AtomicBitPool_AddResultCodes_Medium) { testAddResultCodes(4096); }
TEST(AtomicBitPool_AddResultCodes_Large)  { testAddResultCodes(8192); }

// ============================================================
// add: fill the pool, verify all-AlreadyPresent, then empty and
//      confirm AddedToEmpty is returned again on the first re-add.
// ============================================================

static void testAddFillPool(size_t capacity) {
    PoolFixture f(capacity);

    // Add all indices
    for (size_t i = 0; i < capacity; i++) {
        AR expected = (i == 0) ? AR::AddedToEmpty : AR::AddedToNonempty;
        checkedAdd(f, i, expected);
    }

    // Every index is now present; every add should return AlreadyPresent
    for (size_t i = 0; i < capacity; i++) {
        checkedAdd(f, i, AR::AlreadyPresent);
    }

    // Remove all indices
    for (size_t i = 0; i < capacity; i++) {
        RR expected = (i == capacity - 1) ? RR::RemovedAndMadeEmpty
                                          : RR::RemovedAndStayedNonempty;
        checkedRemove(f, i, expected);
    }

    // Pool is empty again — first re-add must return AddedToEmpty
    checkedAdd(f, 0, AR::AddedToEmpty);
}

TEST(AtomicBitPool_AddFillPool_Small)  { testAddFillPool(64); }
TEST(AtomicBitPool_AddFillPool_Medium) { testAddFillPool(4096); }
TEST(AtomicBitPool_AddFillPool_Large)  { testAddFillPool(8192); }

// ============================================================
// remove: result code correctness
// ============================================================

static void testRemoveResultCodes(size_t capacity) {
    PoolFixture f(capacity);

    // Remove from an empty pool → NotPresent
    checkedRemove(f, 0, RR::NotPresent);
    // Remove an index that was never added → NotPresent
    checkedRemove(f, capacity / 2, RR::NotPresent);

    // Add one element, remove it — pool becomes empty
    checkedAdd(f, 5, AR::AddedToEmpty);
    checkedRemove(f, 5, RR::RemovedAndMadeEmpty);

    // Remove the same index again → NotPresent
    checkedRemove(f, 5, RR::NotPresent);

    // Add two elements, remove the first — pool stays non-empty
    checkedAdd(f, 10, AR::AddedToEmpty);
    checkedAdd(f, 20, AR::AddedToNonempty);
    checkedRemove(f, 10, RR::RemovedAndStayedNonempty);

    // Attempt to remove the already-removed index → NotPresent
    checkedRemove(f, 10, RR::NotPresent);

    // Remove the last remaining element → MadeEmpty
    checkedRemove(f, 20, RR::RemovedAndMadeEmpty);
}

TEST(AtomicBitPool_RemoveResultCodes_Small)  { testRemoveResultCodes(64); }
TEST(AtomicBitPool_RemoveResultCodes_Medium) { testRemoveResultCodes(4096); }
TEST(AtomicBitPool_RemoveResultCodes_Large)  { testRemoveResultCodes(8192); }

// ============================================================
// remove: add all, then remove in forward order
// After the last remove, pool must report AddedToEmpty on re-add.
// ============================================================

static void testAddRemoveAllInOrder(size_t capacity) {
    PoolFixture f(capacity);

    for (size_t i = 0; i < capacity; i++)
        f.pool->add(i);
    ASSERT_TRUE(f.pool->checkInvariants());

    for (size_t i = 0; i < capacity; i++) {
        RR expected = (i == capacity - 1) ? RR::RemovedAndMadeEmpty
                                          : RR::RemovedAndStayedNonempty;
        checkedRemove(f, i, expected);
    }

    // All counts must be zero after full removal
    ASSERT_EQ(f.pool->add(0), AR::AddedToEmpty);
    ASSERT_TRUE(f.pool->checkInvariants());
}

TEST(AtomicBitPool_AddRemoveAllInOrder_Small)  { testAddRemoveAllInOrder(64); }
TEST(AtomicBitPool_AddRemoveAllInOrder_Medium) { testAddRemoveAllInOrder(4096); }
TEST(AtomicBitPool_AddRemoveAllInOrder_Large)  { testAddRemoveAllInOrder(8192); }

// ============================================================
// remove: add all, then remove in reverse order
// ============================================================

static void testAddRemoveAllInReverse(size_t capacity) {
    PoolFixture f(capacity);

    for (size_t i = 0; i < capacity; i++)
        f.pool->add(i);
    ASSERT_TRUE(f.pool->checkInvariants());

    for (size_t i = capacity; i-- > 0;) {
        RR expected = (i == 0) ? RR::RemovedAndMadeEmpty
                               : RR::RemovedAndStayedNonempty;
        checkedRemove(f, i, expected);
    }

    ASSERT_EQ(f.pool->add(0), AR::AddedToEmpty);
    ASSERT_TRUE(f.pool->checkInvariants());
}

TEST(AtomicBitPool_AddRemoveAllInReverse_Small)  { testAddRemoveAllInReverse(64); }
TEST(AtomicBitPool_AddRemoveAllInReverse_Medium) { testAddRemoveAllInReverse(4096); }
TEST(AtomicBitPool_AddRemoveAllInReverse_Large)  { testAddRemoveAllInReverse(8192); }

// ============================================================
// Edge cases
// ============================================================

// capacity == 1: only index 0 is valid
TEST(AtomicBitPool_EdgeCase_Capacity1) {
    PoolFixture f(1);
    checkedAdd(f, 0, AR::AddedToEmpty);
    checkedAdd(f, 0, AR::AlreadyPresent);
    checkedRemove(f, 0, RR::RemovedAndMadeEmpty);
    checkedRemove(f, 0, RR::NotPresent);
}

// capacity == 64: upper boundary for levelCount == 1
TEST(AtomicBitPool_EdgeCase_Capacity64) {
    PoolFixture f(64);
    checkedAdd(f, 0,  AR::AddedToEmpty);
    checkedAdd(f, 63, AR::AddedToNonempty);
    checkedRemove(f, 0,  RR::RemovedAndStayedNonempty);
    checkedRemove(f, 63, RR::RemovedAndMadeEmpty);
}

// capacity == 65: first capacity requiring levelCount == 2
TEST(AtomicBitPool_EdgeCase_Capacity65) {
    // Internal capacity rounds up to 128; index 64 is valid.
    PoolFixture f(65);
    checkedAdd(f, 0,  AR::AddedToEmpty);
    checkedAdd(f, 64, AR::AddedToNonempty);
    checkedRemove(f, 0,  RR::RemovedAndStayedNonempty);
    checkedRemove(f, 64, RR::RemovedAndMadeEmpty);
}

// ============================================================
// Concurrent stress tests
//
// Pattern per epoch:
//   1. Spin up numThreads workers; each picks random indices and
//      alternates add/remove until it sees the stop flag.
//   2. Main thread sleeps epochMs, then sets the stop flag.
//   3. Join all workers — guarantees no operation is in-flight.
//   4. checkInvariants() — safe to call single-threaded here.
//
// The pool is kept across epochs so accumulated state carries over.
// We use 8 threads to match the SMP configuration the kernel targets.
// ============================================================

static void runConcurrentStress(size_t capacity) {
    constexpr int numThreads = 8;
    constexpr int numEpochs  = 50;
    constexpr int epochMs    = 10;

    PoolFixture f(capacity);

    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        pauseTracking();
        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                // Seed each thread differently so they don't all hit the same indices.
                std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(t) << 32));
                std::uniform_int_distribution<size_t> pick(0, capacity - 1);

                while (!stop.load(std::memory_order_relaxed)) {
                    size_t idx = pick(rng);
                    if (rng() & 1)
                        f.pool->add(idx);
                    else
                        f.pool->remove(idx);
                }
            });
        }
        resumeTracking();

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        pauseTracking();
        for (auto& th : threads) th.join();
        resumeTracking();

        ASSERT_TRUE(f.pool->checkInvariants());
    }
}

TEST_WITH_TIMEOUT(AtomicBitPool_ConcurrentStress_Capacity64,   5000) { runConcurrentStress(64); }
TEST_WITH_TIMEOUT(AtomicBitPool_ConcurrentStress_Capacity256,  5000) { runConcurrentStress(256); }
TEST_WITH_TIMEOUT(AtomicBitPool_ConcurrentStress_Capacity8192, 5000) { runConcurrentStress(8192); }

// ============================================================
// getAny: basic result codes
// ============================================================

static void testGetAnyResultCodes(size_t capacity) {
    PoolFixture f(capacity);

    // Empty pool → Empty
    checkedGetAny(f, 0, GR::Empty);

    // One element → Success; the returned index equals the one we added
    checkedAdd(f, 0, AR::AddedToEmpty);
    size_t idx = checkedGetAnySuccess(f, 0);
    ASSERT_EQ(idx, size_t(0));

    // The claimed index is gone from the pool
    checkedRemove(f, idx, RR::NotPresent);

    // Pool is empty again
    checkedGetAny(f, 0, GR::Empty);

    // maxRetries=0 on a non-empty pool: Success or Contended, never Empty
    checkedAdd(f, 0, AR::AddedToEmpty);
    size_t outIdx = SIZE_MAX;
    GR result = f.pool->getAny(0, outIdx, 0);
    ASSERT_TRUE(result == GR::Success || result == GR::Contended);
    ASSERT_TRUE(f.pool->checkInvariants());
}

TEST(AtomicBitPool_GetAnyResultCodes_Small)  { testGetAnyResultCodes(64); }
TEST(AtomicBitPool_GetAnyResultCodes_Medium) { testGetAnyResultCodes(4096); }
TEST(AtomicBitPool_GetAnyResultCodes_Large)  { testGetAnyResultCodes(8192); }

// ============================================================
// getAny: index validity
// Add all indices, drain entirely via getAny, verify every
// returned index is in-range and unique.
// ============================================================

static void testGetAnyDrainAll(size_t capacity) {
    PoolFixture f(capacity);

    for (size_t i = 0; i < capacity; i++)
        f.pool->add(i);
    ASSERT_TRUE(f.pool->checkInvariants());

    std::vector<bool> seen(capacity, false);
    for (size_t i = 0; i < capacity; i++) {
        size_t idx = checkedGetAnySuccess(f, i);
        ASSERT_LT(idx, capacity);
        ASSERT_FALSE(seen[idx]);
        seen[idx] = true;
    }

    // Pool is empty; next add returns AddedToEmpty
    checkedGetAny(f, 0, GR::Empty);
    ASSERT_EQ(f.pool->add(0), AR::AddedToEmpty);
    ASSERT_TRUE(f.pool->checkInvariants());
}

TEST(AtomicBitPool_GetAnyDrainAll_Small)  { testGetAnyDrainAll(64); }
TEST(AtomicBitPool_GetAnyDrainAll_Medium) { testGetAnyDrainAll(4096); }
TEST(AtomicBitPool_GetAnyDrainAll_Large)  { testGetAnyDrainAll(8192); }

// ============================================================
// getAny: interleaved add/getAny
// ============================================================

static void testGetAnyInterleaved(size_t capacity) {
    PoolFixture f(capacity);

    // Add 10 elements, getAny 5 times → 5 distinct indices all in [0, 10)
    for (size_t i = 0; i < 10; i++)
        f.pool->add(i);
    ASSERT_TRUE(f.pool->checkInvariants());

    std::vector<bool> claimed(capacity, false);
    for (size_t i = 0; i < 5; i++) {
        size_t idx = checkedGetAnySuccess(f, i);
        ASSERT_LT(idx, size_t(10));
        ASSERT_FALSE(claimed[idx]);
        claimed[idx] = true;
    }

    // Alternate single-element pool: getAny must return the one added element.
    // With exactly one bit set, doRotation always resolves to that bit regardless
    // of the rotation amount, so this is a deterministic guarantee.
    PoolFixture g(capacity);
    for (size_t i = 0; i < 20; i++) {
        size_t addIdx = i % capacity;
        ASSERT_EQ(g.pool->add(addIdx), AR::AddedToEmpty);
        ASSERT_TRUE(g.pool->checkInvariants());
        size_t gotIdx = checkedGetAnySuccess(g, i);
        ASSERT_EQ(gotIdx, addIdx);
    }
}

TEST(AtomicBitPool_GetAnyInterleaved_Small)  { testGetAnyInterleaved(64); }
TEST(AtomicBitPool_GetAnyInterleaved_Medium) { testGetAnyInterleaved(4096); }
TEST(AtomicBitPool_GetAnyInterleaved_Large)  { testGetAnyInterleaved(8192); }

// ============================================================
// getAny: multiple thread IDs
// Drain a pool using varied threadId values; every call must
// succeed and produce a fresh, in-range, unique index.
// ============================================================

static void testGetAnyMultipleThreadIds(size_t capacity) {
    PoolFixture f(capacity);

    // 32 elements is enough to exercise many different rotation values
    constexpr size_t numElements = 32;
    for (size_t i = 0; i < numElements; i++)
        f.pool->add(i);
    ASSERT_TRUE(f.pool->checkInvariants());

    std::vector<bool> claimed(capacity, false);
    for (size_t t = 0; t < numElements; t++) {
        size_t idx = checkedGetAnySuccess(f, t * 997 + 1);
        ASSERT_LT(idx, numElements);
        ASSERT_FALSE(claimed[idx]);
        claimed[idx] = true;
    }
}

TEST(AtomicBitPool_GetAnyMultipleThreadIds_Small)  { testGetAnyMultipleThreadIds(64); }
TEST(AtomicBitPool_GetAnyMultipleThreadIds_Medium) { testGetAnyMultipleThreadIds(4096); }
TEST(AtomicBitPool_GetAnyMultipleThreadIds_Large)  { testGetAnyMultipleThreadIds(8192); }

// ============================================================
// getAny: edge cases
// ============================================================

TEST(AtomicBitPool_GetAny_EdgeCase_Capacity1) {
    PoolFixture f(1);
    checkedGetAny(f, 0, GR::Empty);
    checkedAdd(f, 0, AR::AddedToEmpty);
    size_t idx = checkedGetAnySuccess(f, 0);
    ASSERT_EQ(idx, size_t(0));
    checkedGetAny(f, 0, GR::Empty);
}

TEST(AtomicBitPool_GetAny_EdgeCase_Capacity64) { testGetAnyDrainAll(64); }
TEST(AtomicBitPool_GetAny_EdgeCase_Capacity65) { testGetAnyDrainAll(65); }

// ============================================================
// getAny: concurrent stress
//
// Each thread randomly picks add, remove, or getAny (1/3 each).
// A per-epoch bitset (one atomic word per 64 indices) shadows the
// pool membership for diagnostic purposes.
// The hard correctness check is checkInvariants() after each epoch.
// ============================================================

static void runGetAnyConcurrentStress(size_t capacity) {
    constexpr int numThreads = 8;
    constexpr int numEpochs  = 50;
    constexpr int epochMs    = 10;

    PoolFixture f(capacity);
    size_t numWords = capacity / 64;

    size_t totalSuccess = 0, totalEmpty = 0, totalContended = 0;

    for (int epoch = 0; epoch < numEpochs; epoch++) {
        std::atomic<bool>   stop{false};
        std::atomic<size_t> successCount{0};
        std::atomic<size_t> emptyCount{0};
        std::atomic<size_t> contendedCount{0};

        // Shadow bitset: tracks which indices are currently in the pool.
        auto bitset = std::make_unique<std::atomic<uint64_t>[]>(numWords);
        for (size_t i = 0; i < numWords; i++) bitset[i].store(0, std::memory_order_relaxed);

        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        pauseTracking();
        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(t) << 32));
                std::uniform_int_distribution<size_t> pickIdx(0, capacity - 1);
                std::uniform_int_distribution<int>    pickOp(0, 2);

                while (!stop.load(std::memory_order_relaxed)) {
                    size_t idx = pickIdx(rng);
                    int    op  = pickOp(rng);

                    if (op == 0) {
                        AR result = f.pool->add(idx);
                        if (result != AR::AlreadyPresent)
                            bitset[idx / 64].fetch_or(uint64_t(1) << (idx % 64),
                                                      std::memory_order_relaxed);
                    } else if (op == 1) {
                        RR result = f.pool->remove(idx);
                        if (result != RR::NotPresent)
                            bitset[idx / 64].fetch_and(~(uint64_t(1) << (idx % 64)),
                                                       std::memory_order_relaxed);
                    } else {
                        size_t outIdx = SIZE_MAX;
                        GR result = f.pool->getAny(size_t(t), outIdx);
                        if (result == GR::Success) {
                            successCount.fetch_add(1, std::memory_order_relaxed);
                            bitset[outIdx / 64].fetch_and(
                                ~(uint64_t(1) << (outIdx % 64)),
                                std::memory_order_relaxed);
                        } else if (result == GR::Empty) {
                            emptyCount.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            contendedCount.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }
        resumeTracking();

        std::this_thread::sleep_for(std::chrono::milliseconds(epochMs));
        stop.store(true, std::memory_order_relaxed);
        pauseTracking();
        for (auto& th : threads) th.join();
        resumeTracking();

        ASSERT_TRUE(f.pool->checkInvariants());
        totalSuccess   += successCount.load();
        totalEmpty     += emptyCount.load();
        totalContended += contendedCount.load();
    }
    printf("  %d epochs: success=%zu empty=%zu contended=%zu\n",
           numEpochs, totalSuccess, totalEmpty, totalContended);
    fflush(stdout);
}

TEST_WITH_TIMEOUT(AtomicBitPool_GetAnyConcurrentStress_Capacity64,   5000) { runGetAnyConcurrentStress(64); }
TEST_WITH_TIMEOUT(AtomicBitPool_GetAnyConcurrentStress_Capacity256,  5000) { runGetAnyConcurrentStress(256); }
TEST_WITH_TIMEOUT(AtomicBitPool_GetAnyConcurrentStress_Capacity8192, 5000) { runGetAnyConcurrentStress(8192); }

// ============================================================
// Emptiness transitions: AddedToEmpty / RemovedAndMadeEmpty
//
// Explicitly exercises the LN bitmap-derived emptiness signal for
// each pool depth: levelCount==1 (cap 64), levelCount==2 (cap 128),
// and levelCount==3 (cap 8192).
// ============================================================

static void testEmptinessTransitions(size_t capacity) {
    PoolFixture f(capacity);

    // Empty pool: first add must return AddedToEmpty
    checkedAdd(f, 0, AR::AddedToEmpty);
    // Second add to a non-empty pool: AddedToNonempty
    checkedAdd(f, 1, AR::AddedToNonempty);
    // Remove one, pool stays nonempty
    checkedRemove(f, 0, RR::RemovedAndStayedNonempty);
    // Remove last element: pool becomes empty
    checkedRemove(f, 1, RR::RemovedAndMadeEmpty);

    // Pool is empty again; next add must return AddedToEmpty
    checkedAdd(f, capacity - 1, AR::AddedToEmpty);
    checkedRemove(f, capacity - 1, RR::RemovedAndMadeEmpty);
}

TEST(AtomicBitPool_EmptinessTransitions_LevelCount1) { testEmptinessTransitions(64); }
TEST(AtomicBitPool_EmptinessTransitions_LevelCount2) { testEmptinessTransitions(128); }
TEST(AtomicBitPool_EmptinessTransitions_LevelCount3) { testEmptinessTransitions(8192); }