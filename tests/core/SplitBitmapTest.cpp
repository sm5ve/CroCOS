//
// Unit tests for Core::SplitBitmap
// Created by Spencer Martin on 5/20/26.
//
// Coverage:
//   - Inline storage, single-word and multi-word; with and without hint
//   - External storage, single-word
//   - seedAllAvailable / seedAllUsed round-trips
//   - claimBit drains free → alloc when alloc side empties
//   - tryClaimBitNoDrain returns -1 on alloc-empty without touching free side
//   - releaseBit reports wordWasZero edge correctly
//   - releaseBitsBulk parity with per-bit releaseBit
//   - reserveBit removes from circulation
//   - flushAllocIntoFree round-trips alloc-side bits to free side
//   - countAvailable / isBitAvailable / allocSideEmpty
//   - Multi-threaded one-alloc-CPU + many-freer-CPU race: every release
//     eventually returns through claimBit
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/atomic/SplitBitmap.h>

#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <random>
#include <cstdint>

using Core::SplitBitmap;
using Core::InlineSplitBitmapStorage;
using Core::ExternalSplitBitmapStorage;

// ============================================================
// Inline storage, W=1 (no hint)
// ============================================================

TEST(SplitBitmap_Inline_W1_SeedAllAvailable_64Bits) {
    SplitBitmap<1> bm;
    bm.seedAllAvailable();
    ASSERT_EQ(size_t(64), bm.countAvailable());
    ASSERT_FALSE(bm.allocSideEmpty());
}

TEST(SplitBitmap_Inline_W1_SeedAllUsed_NoBits) {
    SplitBitmap<1> bm;
    bm.seedAllUsed();
    ASSERT_EQ(size_t(0), bm.countAvailable());
    ASSERT_TRUE(bm.allocSideEmpty());
}

TEST(SplitBitmap_Inline_W1_ClaimDrains64Bits) {
    SplitBitmap<1> bm;
    bm.seedAllAvailable();
    std::set<int> claimed;
    for (size_t i = 0; i < 64; i++) {
        int bit = bm.claimBit();
        ASSERT_GE(bit, 0);
        ASSERT_LT(bit, 64);
        ASSERT_EQ(size_t(0), claimed.count(bit));
        claimed.insert(bit);
    }
    ASSERT_EQ(-1, bm.claimBit());
    ASSERT_EQ(size_t(64), claimed.size());
}

TEST(SplitBitmap_Inline_W1_ReleaseRoundTrip) {
    SplitBitmap<1> bm;
    bm.seedAllAvailable();
    int b0 = bm.claimBit();
    int b1 = bm.claimBit();
    int b2 = bm.claimBit();
    ASSERT_EQ(size_t(61), bm.countAvailable());

    bm.releaseBit(b0);
    bm.releaseBit(b1);
    bm.releaseBit(b2);
    ASSERT_EQ(size_t(64), bm.countAvailable());

    std::set<int> reclaimed;
    for (size_t i = 0; i < 64; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
        reclaimed.insert(b);
    }
    ASSERT_EQ(size_t(64), reclaimed.size());
}

TEST(SplitBitmap_Inline_W1_TryClaimNoDrainDoesNotDrain) {
    SplitBitmap<1> bm;
    bm.seedAllUsed();
    bm.releaseBit(5);
    ASSERT_EQ(-1, bm.tryClaimBitNoDrain());
    // tryClaimBitNoDrain must not have moved anything; the bit still lives
    // on the free side. countAvailable sees it.
    ASSERT_EQ(size_t(1), bm.countAvailable());
}

TEST(SplitBitmap_Inline_W1_DrainExposesFreedBits) {
    SplitBitmap<1> bm;
    bm.seedAllUsed();
    bm.releaseBit(7);
    bm.releaseBit(13);
    ASSERT_TRUE(bm.drainFreeIntoAlloc());
    int a = bm.claimBit();
    int b = bm.claimBit();
    ASSERT_TRUE((a == 7  && b == 13) || (a == 13 && b == 7));
    ASSERT_EQ(-1, bm.claimBit());
}

TEST(SplitBitmap_Inline_W1_ReleaseEdgeReport) {
    SplitBitmap<1> bm;
    bm.seedAllUsed();
    auto r0 = bm.releaseBit(3);
    ASSERT_TRUE(r0.wordWasZero);
    auto r1 = bm.releaseBit(4);
    ASSERT_FALSE(r1.wordWasZero);
}

TEST(SplitBitmap_Inline_W1_ReserveBit) {
    SplitBitmap<1> bm;
    bm.seedAllAvailable();
    bm.reserveBit(0);
    bm.reserveBit(63);
    ASSERT_EQ(size_t(62), bm.countAvailable());
    ASSERT_FALSE(bm.isBitAvailable(0));
    ASSERT_FALSE(bm.isBitAvailable(63));
    ASSERT_TRUE(bm.isBitAvailable(1));
}

TEST(SplitBitmap_Inline_W1_FlushAllocIntoFree) {
    SplitBitmap<1> bm;
    bm.seedAllAvailable();
    // Claim two bits, then flush. Remaining alloc-side bits should land in
    // free side, and the next claim must drain to find them.
    int x = bm.claimBit();
    int y = bm.claimBit();
    (void)x; (void)y;
    ASSERT_EQ(size_t(62), bm.countAvailable());

    bm.flushAllocIntoFree();
    ASSERT_TRUE(bm.allocSideEmpty());
    ASSERT_EQ(size_t(62), bm.countAvailable());

    // tryClaimBitNoDrain must miss; claimBit must succeed via drain.
    ASSERT_EQ(-1, bm.tryClaimBitNoDrain());
    int z = bm.claimBit();
    ASSERT_GE(z, 0);
}

// ============================================================
// Inline storage, W=8 (multi-word, with hint)
// ============================================================

TEST(SplitBitmap_Inline_W8_SeedAllAvailable_512Bits) {
    SplitBitmap<8> bm;
    bm.seedAllAvailable();
    ASSERT_EQ(size_t(512), bm.countAvailable());
}

TEST(SplitBitmap_Inline_W8_ClaimAllBitsExactlyOnce) {
    SplitBitmap<8> bm;
    bm.seedAllAvailable();
    std::set<int> claimed;
    for (size_t i = 0; i < 512; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
        ASSERT_LT(b, 512);
        ASSERT_EQ(size_t(0), claimed.count(b));
        claimed.insert(b);
    }
    ASSERT_EQ(-1, bm.claimBit());
    ASSERT_EQ(size_t(512), claimed.size());
}

TEST(SplitBitmap_Inline_W8_DrainExposesCrossWordBits) {
    SplitBitmap<8> bm;
    bm.seedAllUsed();
    // Release bits in words 0, 3, and 7 so the drain has to update the hint
    // to the lowest non-empty word (0).
    bm.releaseBit(2);    // word 0
    bm.releaseBit(200);  // word 3
    bm.releaseBit(500);  // word 7
    int a = bm.claimBit();
    int b = bm.claimBit();
    int c = bm.claimBit();
    std::set<int> got{a, b, c};
    ASSERT_EQ(size_t(3), got.size());
    ASSERT_EQ(size_t(1), got.count(2));
    ASSERT_EQ(size_t(1), got.count(200));
    ASSERT_EQ(size_t(1), got.count(500));
    ASSERT_EQ(-1, bm.claimBit());
}

TEST(SplitBitmap_Inline_W8_HintAdvancesPastEmptyWords) {
    SplitBitmap<8> bm;
    bm.seedAllUsed();
    bm.releaseBit(400);   // word 6
    bm.drainFreeIntoAlloc();
    // After drain, only word 6 has bits. Claim should still find it.
    int x = bm.claimBit();
    ASSERT_EQ(400, x);
}

TEST(SplitBitmap_Inline_W8_ReleaseBitsBulkEquivalent) {
    SplitBitmap<8> bm;
    bm.seedAllUsed();
    // Mark some bits via the bulk API.
    uint64_t pending[8] = {};
    pending[0] = (uint64_t{1} << 5) | (uint64_t{1} << 10);
    pending[3] = uint64_t{1} << 1;
    pending[7] = uint64_t{1} << 63;
    bm.releaseBitsBulk(pending);
    ASSERT_EQ(size_t(4), bm.countAvailable());
    std::set<int> got;
    for (size_t i = 0; i < 4; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
        got.insert(b);
    }
    ASSERT_EQ(size_t(1), got.count(5));
    ASSERT_EQ(size_t(1), got.count(10));
    ASSERT_EQ(size_t(1), got.count(64 * 3 + 1));
    ASSERT_EQ(size_t(1), got.count(64 * 7 + 63));
    ASSERT_EQ(-1, bm.claimBit());
}

TEST(SplitBitmap_Inline_W8_FlushThenClaim) {
    SplitBitmap<8> bm;
    bm.seedAllAvailable();
    // Claim a handful, flush, then claim all remaining.
    for (size_t i = 0; i < 10; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
    }
    bm.flushAllocIntoFree();
    ASSERT_TRUE(bm.allocSideEmpty());
    ASSERT_EQ(size_t(502), bm.countAvailable());

    std::set<int> reclaimed;
    for (size_t i = 0; i < 502; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
        reclaimed.insert(b);
    }
    ASSERT_EQ(size_t(502), reclaimed.size());
    ASSERT_EQ(-1, bm.claimBit());
}

// ============================================================
// External storage, W=1 (VMSubstrate-style)
// ============================================================

TEST(SplitBitmap_External_W1_RoundTrip) {
    // Caller-managed storage: alloc and free buffers live separately, just
    // like VMSubstrate's occupancy buffer layout.
    alignas(64) uint64_t allocBuf[1] = {0};
    alignas(64) Atomic<uint64_t> freeBuf[1] = {};

    SplitBitmap<1, ExternalSplitBitmapStorage<1>, false> bm(allocBuf, freeBuf);
    bm.seedAllAvailable();
    ASSERT_EQ(size_t(64), bm.countAvailable());

    std::set<int> got;
    for (size_t i = 0; i < 64; i++) {
        int b = bm.claimBit();
        ASSERT_GE(b, 0);
        got.insert(b);
    }
    ASSERT_EQ(size_t(64), got.size());
    ASSERT_EQ(-1, bm.claimBit());

    // Concurrent freer simulation: ORs into freeBuf directly via the bm view.
    auto r = bm.releaseBit(42);
    ASSERT_TRUE(r.wordWasZero);
    ASSERT_EQ(size_t(1), bm.countAvailable());
    int b = bm.claimBit();
    ASSERT_EQ(42, b);
}

TEST(SplitBitmap_External_W1_StorageIsExternal) {
    // Sanity: the view does not own the storage; multiple views over the
    // same buffers see each other's writes.
    alignas(64) uint64_t allocBuf[1] = {0};
    alignas(64) Atomic<uint64_t> freeBuf[1] = {};

    {
        SplitBitmap<1, ExternalSplitBitmapStorage<1>, false> seeder(allocBuf, freeBuf);
        seeder.seedAllAvailable();
    }

    SplitBitmap<1, ExternalSplitBitmapStorage<1>, false> reader(allocBuf, freeBuf);
    ASSERT_EQ(size_t(64), reader.countAvailable());
}

// ============================================================
// Multi-threaded: one alloc CPU + many freer CPUs
// ============================================================

// Single allocator drains and re-publishes; multiple freers concurrently
// release distinct bits. Every released bit eventually appears at the
// allocator. Inherently multi-threaded → no memory tracking.
TEST_WITH_TIMEOUT_NO_TRACKING(SplitBitmap_MultiThread_OneAllocManyFreers, 5000) {
    constexpr size_t W = 8;
    constexpr size_t kBits = W * 64;
    constexpr size_t kFreerThreads = 4;
    constexpr size_t kRoundsPerThread = 5000;

    SplitBitmap<W> bm;
    bm.seedAllAvailable();

    // The allocator thread claims bits, hands them off to freers via per-freer
    // SPSC queues (using std::atomic indices), and waits for completion. Each
    // freer releases received bits.

    std::vector<std::vector<std::atomic<int>>> outboxes(kFreerThreads);
    for (auto& q : outboxes) {
        q = std::vector<std::atomic<int>>(kRoundsPerThread);
        for (auto& slot : q) slot.store(-1, std::memory_order_relaxed);
    }

    std::atomic<bool> allocatorDone{false};
    std::atomic<size_t> totalClaimed{0};

    std::thread allocator([&] {
        size_t nextSlot[kFreerThreads] = {};
        std::vector<std::array<int, 64>> pending;
        size_t roundsRemaining = kFreerThreads * kRoundsPerThread;
        while (roundsRemaining > 0) {
            int bit = bm.claimBit();
            if (bit < 0) {
                std::this_thread::yield();
                continue;
            }
            const size_t freer = totalClaimed.fetch_add(1) % kFreerThreads;
            // Wait until the freer has consumed the next slot.
            while (outboxes[freer][nextSlot[freer]].load(std::memory_order_acquire) != -1)
                std::this_thread::yield();
            outboxes[freer][nextSlot[freer]].store(bit, std::memory_order_release);
            nextSlot[freer]++;
            roundsRemaining--;
        }
        allocatorDone.store(true, std::memory_order_release);
    });

    std::vector<std::thread> freers;
    freers.reserve(kFreerThreads);
    for (size_t f = 0; f < kFreerThreads; f++) {
        freers.emplace_back([&, f] {
            for (size_t i = 0; i < kRoundsPerThread; i++) {
                int bit = -1;
                while ((bit = outboxes[f][i].load(std::memory_order_acquire)) == -1) {
                    std::this_thread::yield();
                }
                bm.releaseBit(static_cast<size_t>(bit));
                outboxes[f][i].store(-1, std::memory_order_release);  // consumed
            }
        });
    }

    allocator.join();
    for (auto& t : freers) t.join();

    // Drain any remaining published bits back into the alloc side.
    while (bm.drainFreeIntoAlloc()) {}

    // The pool should now have all kBits bits available again.
    ASSERT_EQ(kBits, bm.countAvailable());
    ASSERT_EQ(kFreerThreads * kRoundsPerThread, totalClaimed.load());
}
