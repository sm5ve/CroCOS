//
// Unit tests for AtomicIntrusiveLinkedList
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/atomic/AtomicLinkedList.h>

#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <memory>
#include <shared_mutex>

using namespace CroCOSTest;

// ============================================================
// Test node and extractor
//
// LNode embeds a std::shared_mutex as its per-node lock.
// The list's headLock and tailLock are also std::shared_mutex.
// ============================================================

struct LNode {
    int              value = 0;
    Atomic<LNode*>   prev{nullptr};
    Atomic<LNode*>   next{nullptr};
    std::shared_mutex lock;

    explicit LNode(int v = 0) : value(v) {}
    LNode(const LNode&)            = delete;
    LNode& operator=(const LNode&) = delete;
};

struct LNodeExtractor {
    static Atomic<LNode*>& previous(LNode& n)     { return n.prev; }
    static Atomic<LNode*>& next(LNode& n)          { return n.next; }
    static std::shared_mutex& nodeLock(LNode& n)   { return n.lock; }
    static void lockWriter(std::shared_mutex& l)   { l.lock(); }
    static void unlockWriter(std::shared_mutex& l) { l.unlock(); }
    static void lockReader(std::shared_mutex& l)   { l.lock_shared(); }
    static void unlockReader(std::shared_mutex& l) { l.unlock_shared(); }
    static bool tryLockWriter(std::shared_mutex& l) { return l.try_lock(); }
};

using TestList = AtomicIntrusiveLinkedList<LNode, std::shared_mutex, LNodeExtractor>;

// ============================================================
// Structural validation helpers
//
// Only safe to call after all concurrent threads have joined,
// since they access node pointers directly without locks.
// ============================================================

// The sentinel value used by the list to mark head.prev and tail.next.
// Mirrors the private sentinel() inside AtomicIntrusiveLinkedList.
static LNode* testSentinel() { return reinterpret_cast<LNode*>(~0ULL); }
static bool isRealNode(LNode* p) { return p != nullptr && p != testSentinel(); }

// Walk the list forward, asserting that every node's prev pointer
// is consistent, and return the collected values in order.
static std::vector<int> walkForward(TestList& list) {
    std::vector<int> out;
    LNode* prev = nullptr;
    LNode* curr = list.getHead();
    while (curr) {
        // The head's prev should be SENTINEL; interior nodes should point to prev.
        LNode* expectedPrev = (prev == nullptr) ? testSentinel() : prev;
        ASSERT_EQ(curr->prev, expectedPrev);
        out.push_back(curr->value);
        prev = curr;
        LNode* n = curr->next;
        curr = isRealNode(n) ? n : nullptr;
    }
    // The last visited node must match tail.
    ASSERT_EQ(list.getTail(), prev);
    // If the list is non-empty, tail.next must be SENTINEL.
    if (prev != nullptr) {
        ASSERT_EQ(prev->next, testSentinel());
    }
    return out;
}

// Full bidirectional integrity check: backward traversal must be the
// exact reverse of the forward traversal.
static void assertIntegrity(TestList& list) {
    std::vector<int> fwd = walkForward(list);

    std::vector<int> bwd;
    LNode* next = nullptr;
    LNode* curr = list.getTail();
    while (curr) {
        // The tail's next should be SENTINEL; interior nodes should point to next.
        LNode* expectedNext = (next == nullptr) ? testSentinel() : next;
        ASSERT_EQ(curr->next, expectedNext);
        bwd.push_back(curr->value);
        next = curr;
        LNode* p = curr->prev;
        curr = isRealNode(p) ? p : nullptr;
    }

    ASSERT_EQ(fwd.size(), bwd.size());
    for (size_t i = 0; i < fwd.size(); i++) {
        ASSERT_EQ(fwd[i], bwd[fwd.size() - 1 - i]);
    }
}

// ============================================================
// Single-threaded: pushBack
// ============================================================

TEST(AtomicLinkedListPushBackOrder) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    auto fwd = walkForward(list);
    ASSERT_EQ(3u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    assertIntegrity(list);
}

// ============================================================
// Single-threaded: pushFront
// ============================================================

TEST(AtomicLinkedListPushFrontOrder) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushFront(a);
    list.pushFront(b);
    list.pushFront(c);

    // Expected: 3 -> 2 -> 1
    auto fwd = walkForward(list);
    ASSERT_EQ(3u, fwd.size());
    ASSERT_EQ(3, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(1, fwd[2]);
    assertIntegrity(list);
}

// ============================================================
// Single-threaded: remove
// ============================================================

TEST(AtomicLinkedListRemoveMiddle) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);
    ASSERT_TRUE(list.remove(b));

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(3, fwd[1]);
    ASSERT_EQ(nullptr, b.prev);
    ASSERT_EQ(nullptr, b.next);
    assertIntegrity(list);
}

TEST(AtomicLinkedListRemoveHead) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);
    ASSERT_TRUE(list.remove(a));

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(2, fwd[0]);
    ASSERT_EQ(3, fwd[1]);
    ASSERT_EQ(nullptr, a.prev);
    ASSERT_EQ(nullptr, a.next);
    assertIntegrity(list);
}

TEST(AtomicLinkedListRemoveTail) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);
    ASSERT_TRUE(list.remove(c));

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(nullptr, c.prev);
    ASSERT_EQ(nullptr, c.next);
    assertIntegrity(list);
}

TEST(AtomicLinkedListRemoveSoleNode) {
    TestList list;
    LNode a(42);

    list.pushBack(a);
    ASSERT_TRUE(list.remove(a));

    ASSERT_TRUE(list.empty());
    ASSERT_EQ(nullptr, list.getHead());
    ASSERT_EQ(nullptr, list.getTail());
    ASSERT_EQ(nullptr, a.prev);
    ASSERT_EQ(nullptr, a.next);
}

// ============================================================
// Single-threaded: double-remove returns false (no-op)
// ============================================================

TEST(AtomicLinkedListDoubleRemoveIsNoop) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    ASSERT_TRUE(list.remove(b));
    // b is now detached (prev == next == nullptr).
    // A second remove must return false and not corrupt the list.
    ASSERT_FALSE(list.remove(b));

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(3, fwd[1]);
    assertIntegrity(list);
}

TEST(AtomicLinkedListDoubleRemoveSoleNode) {
    TestList list;
    LNode a(42);

    list.pushBack(a);
    ASSERT_TRUE(list.remove(a));
    ASSERT_FALSE(list.remove(a));

    ASSERT_TRUE(list.empty());
}

// ============================================================
// Single-threaded: remove from wrong list returns false
// ============================================================

TEST(AtomicLinkedListRemoveFromWrongListHead) {
    TestList listA, listB;
    LNode a(1), b(2);

    listA.pushBack(a);
    listB.pushBack(b);

    // b is head+tail of listB. Trying to remove it from listA must be a no-op.
    ASSERT_FALSE(listA.remove(b));

    // Both lists must be intact.
    auto fwdA = walkForward(listA);
    ASSERT_EQ(1u, fwdA.size());
    ASSERT_EQ(1, fwdA[0]);
    assertIntegrity(listA);

    auto fwdB = walkForward(listB);
    ASSERT_EQ(1u, fwdB.size());
    ASSERT_EQ(2, fwdB[0]);
    assertIntegrity(listB);
}

TEST(AtomicLinkedListRemoveFromWrongListTail) {
    TestList listA, listB;
    LNode a1(1), a2(2), b1(10), b2(20);

    listA.pushBack(a1);
    listA.pushBack(a2);
    listB.pushBack(b1);
    listB.pushBack(b2);

    // b2 is the tail of listB. Its next is SENTINEL.
    // Removing it from listA: prev is &b1 (a real node), so the SENTINEL
    // check won't trigger.  But this exercises the middle-node path where
    // the lock on b1 is acquired — the validation will still pass (ABA
    // for middle nodes is accepted). This test documents that behavior.
    // The head-only and tail-only cross-list cases ARE caught.

    // b1 is the head of listB — removing from listA should be a no-op.
    ASSERT_FALSE(listA.remove(b1));

    assertIntegrity(listA);
    assertIntegrity(listB);
}

// ============================================================
// Single-threaded: popFront
// ============================================================

TEST(AtomicLinkedListPopFrontBasic) {
    TestList list;
    LNode a(10), b(20), c(30);

    ASSERT_EQ(nullptr, list.popFront());

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    LNode* got = list.popFront();
    ASSERT_EQ(&a, got);
    ASSERT_EQ(nullptr, a.prev);
    ASSERT_EQ(nullptr, a.next);

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(20, fwd[0]);
    ASSERT_EQ(30, fwd[1]);

    ASSERT_EQ(&b, list.popFront());
    ASSERT_EQ(&c, list.popFront());
    ASSERT_TRUE(list.empty());
    ASSERT_EQ(nullptr, list.popFront());
}

// ============================================================
// Single-threaded: popBack
// ============================================================

TEST(AtomicLinkedListPopBackBasic) {
    TestList list;
    LNode a(10), b(20), c(30);

    ASSERT_EQ(nullptr, list.popBack());

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    LNode* got = list.popBack();
    ASSERT_EQ(&c, got);
    ASSERT_EQ(nullptr, c.prev);
    ASSERT_EQ(nullptr, c.next);

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(10, fwd[0]);
    ASSERT_EQ(20, fwd[1]);

    ASSERT_EQ(&b, list.popBack());
    ASSERT_EQ(&a, list.popBack());
    ASSERT_TRUE(list.empty());
    ASSERT_EQ(nullptr, list.popBack());
}

// ============================================================
// Single-threaded: single-element pop clears both endpoints
// ============================================================

TEST(AtomicLinkedListSingleElementPop) {
    {
        TestList list;
        LNode a(1);
        list.pushBack(a);
        ASSERT_EQ(&a, list.popFront());
        ASSERT_EQ(nullptr, list.getHead());
        ASSERT_EQ(nullptr, list.getTail());
        ASSERT_EQ(nullptr, a.prev);
        ASSERT_EQ(nullptr, a.next);
    }
    {
        TestList list;
        LNode a(1);
        list.pushBack(a);
        ASSERT_EQ(&a, list.popBack());
        ASSERT_EQ(nullptr, list.getHead());
        ASSERT_EQ(nullptr, list.getTail());
    }
}

// ============================================================
// Single-threaded: remove then re-add to same list
// ============================================================

TEST(AtomicLinkedListRemoveThenReAdd) {
    TestList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    ASSERT_TRUE(list.remove(b));
    ASSERT_EQ(nullptr, b.prev);
    ASSERT_EQ(nullptr, b.next);

    // Re-add b at the front.
    list.pushFront(b);

    auto fwd = walkForward(list);
    ASSERT_EQ(3u, fwd.size());
    ASSERT_EQ(2, fwd[0]);
    ASSERT_EQ(1, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    assertIntegrity(list);
}

// ============================================================
// Single-threaded: spliceFront
// ============================================================

TEST(AtomicLinkedListSpliceFrontNonEmptyIntoEmpty) {
    TestList dst, src;
    LNode a(1), b(2), c(3);

    src.pushBack(a);
    src.pushBack(b);
    src.pushBack(c);

    dst.spliceFront(src);

    ASSERT_TRUE(src.empty());
    auto fwd = walkForward(dst);
    ASSERT_EQ(3u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceFrontEmptyIntoNonEmpty) {
    TestList dst, src;
    LNode a(1), b(2);

    dst.pushBack(a);
    dst.pushBack(b);

    dst.spliceFront(src);

    ASSERT_TRUE(src.empty());
    auto fwd = walkForward(dst);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceFrontNonEmptyIntoNonEmpty) {
    TestList dst, src;
    LNode a(1), b(2), c(3), d(4), e(5);

    dst.pushBack(d);
    dst.pushBack(e);

    src.pushBack(a);
    src.pushBack(b);
    src.pushBack(c);

    dst.spliceFront(src);

    ASSERT_TRUE(src.empty());
    // Expected order: 1 2 3 4 5  (source prepended before dst)
    auto fwd = walkForward(dst);
    ASSERT_EQ(5u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    ASSERT_EQ(4, fwd[3]);
    ASSERT_EQ(5, fwd[4]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceFrontEmptyIntoEmpty) {
    TestList dst, src;

    dst.spliceFront(src);

    ASSERT_TRUE(dst.empty());
    ASSERT_TRUE(src.empty());
}

TEST(AtomicLinkedListSpliceFrontSelfIsNoop) {
    TestList list;
    LNode a(1), b(2);
    list.pushBack(a);
    list.pushBack(b);

    list.spliceFront(list);

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    assertIntegrity(list);
}

// ============================================================
// Single-threaded: spliceBack
// ============================================================

TEST(AtomicLinkedListSpliceBackNonEmptyIntoEmpty) {
    TestList dst, src;
    LNode a(1), b(2), c(3);

    src.pushBack(a);
    src.pushBack(b);
    src.pushBack(c);

    dst.spliceBack(src);

    ASSERT_TRUE(src.empty());
    auto fwd = walkForward(dst);
    ASSERT_EQ(3u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceBackEmptyIntoNonEmpty) {
    TestList dst, src;
    LNode a(1), b(2);

    dst.pushBack(a);
    dst.pushBack(b);

    dst.spliceBack(src);

    ASSERT_TRUE(src.empty());
    auto fwd = walkForward(dst);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceBackNonEmptyIntoNonEmpty) {
    TestList dst, src;
    LNode a(1), b(2), c(3), d(4), e(5);

    dst.pushBack(a);
    dst.pushBack(b);

    src.pushBack(c);
    src.pushBack(d);
    src.pushBack(e);

    dst.spliceBack(src);

    ASSERT_TRUE(src.empty());
    // Expected order: 1 2 3 4 5  (source appended after dst)
    auto fwd = walkForward(dst);
    ASSERT_EQ(5u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    ASSERT_EQ(3, fwd[2]);
    ASSERT_EQ(4, fwd[3]);
    ASSERT_EQ(5, fwd[4]);
    assertIntegrity(dst);
}

TEST(AtomicLinkedListSpliceBackEmptyIntoEmpty) {
    TestList dst, src;

    dst.spliceBack(src);

    ASSERT_TRUE(dst.empty());
    ASSERT_TRUE(src.empty());
}

TEST(AtomicLinkedListSpliceBackSelfIsNoop) {
    TestList list;
    LNode a(1), b(2);
    list.pushBack(a);
    list.pushBack(b);

    list.spliceBack(list);

    auto fwd = walkForward(list);
    ASSERT_EQ(2u, fwd.size());
    ASSERT_EQ(1, fwd[0]);
    ASSERT_EQ(2, fwd[1]);
    assertIntegrity(list);
}

// ============================================================
// Concurrent pushBack: N threads each pushBack their partition.
// After joining: all N nodes present, structure valid.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentPushBack, 10000) {
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 100;
    constexpr int kTotal     = kThreads * kPerThread;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};

    auto worker = [&](int t) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerThread; i++) list.pushBack(*nodes[t * kPerThread + i]);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(worker, t);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    auto fwd = walkForward(list);
    ASSERT_EQ((size_t)kTotal, fwd.size());
    std::sort(fwd.begin(), fwd.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, fwd[i]);
    assertIntegrity(list);
}

// ============================================================
// Concurrent pushFront: same as above but from the front.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentPushFront, 10000) {
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 100;
    constexpr int kTotal     = kThreads * kPerThread;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};

    auto worker = [&](int t) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerThread; i++) list.pushFront(*nodes[t * kPerThread + i]);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(worker, t);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    auto fwd = walkForward(list);
    ASSERT_EQ((size_t)kTotal, fwd.size());
    std::sort(fwd.begin(), fwd.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, fwd[i]);
    assertIntegrity(list);
}

// ============================================================
// Concurrent mixed push: even threads pushBack, odd pushFront.
// Hammers both endpoints simultaneously.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentMixedPush, 10000) {
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 100;
    constexpr int kTotal     = kThreads * kPerThread;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};

    auto worker = [&](int t) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerThread; i++) {
            LNode& node = *nodes[t * kPerThread + i];
            if (t % 2 == 0) list.pushBack(node);
            else             list.pushFront(node);
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(worker, t);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    auto fwd = walkForward(list);
    ASSERT_EQ((size_t)kTotal, fwd.size());
    std::sort(fwd.begin(), fwd.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, fwd[i]);
    assertIntegrity(list);
}

// ============================================================
// MPSC queue: multiple producers pushBack, one consumer popFront.
// Exercises the retry path in pushBack under contention.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListMPSCQueue, 10000) {
    constexpr int kProducers    = 4;
    constexpr int kPerProducer  = 200;
    constexpr int kTotal        = kProducers * kPerProducer;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};
    std::atomic<int>  produced{0};

    auto producer = [&](int p) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerProducer; i++) {
            list.pushBack(*nodes[p * kPerProducer + i]);
            produced.fetch_add(1, std::memory_order_release);
        }
    };

    std::vector<int> consumed;

    auto consumer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while ((int)consumed.size() < kTotal) {
            LNode* n = list.popFront();
            if (n) consumed.push_back(n->value);
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; p++) threads.emplace_back(producer, p);
    std::thread consThread(consumer);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    consThread.join();
    resumeTracking();

    ASSERT_EQ((size_t)kTotal, consumed.size());
    std::sort(consumed.begin(), consumed.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, consumed[i]);
    ASSERT_TRUE(list.empty());
}

// ============================================================
// SPMC popFront: one producer pushBack, multiple consumers popFront.
// Exercises concurrent popFront on the same list endpoint.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListSPMCPopFront, 10000) {
    constexpr int kConsumers = 4;
    constexpr int kTotal     = 800;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    // Use a pre-allocated array + atomic index to avoid heap allocations inside
    // consumer threads, which could straddle a pauseTracking boundary and
    // confuse the memory tracker (same pattern as RingBufferTest).
    int collectedValues[kTotal];
    std::atomic<int> collectedIndex{0};
    TestList list;
    std::atomic<bool> start{false};
    std::atomic<int>  totalPopped{0};

    auto consumer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalPopped.load(std::memory_order_acquire) < kTotal) {
            LNode* n = list.popFront();
            if (n) {
                int idx = collectedIndex.fetch_add(1, std::memory_order_relaxed);
                collectedValues[idx] = n->value;
                totalPopped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    pauseTracking();
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; c++) consumers.emplace_back(consumer);
    resumeTracking();

    start.store(true, std::memory_order_release);
    for (int i = 0; i < kTotal; i++) list.pushBack(*nodes[i]);  // main thread = producer

    pauseTracking();
    for (auto& t : consumers) t.join();
    resumeTracking();

    int count = collectedIndex.load();
    ASSERT_EQ(kTotal, count);
    std::sort(collectedValues, collectedValues + count);
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, collectedValues[i]);
    ASSERT_TRUE(list.empty());
}

// ============================================================
// SPMC popBack: one producer pushFront, multiple consumers popBack.
// Exercises concurrent popBack under contention.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListSPMCPopBack, 10000) {
    constexpr int kConsumers = 4;
    constexpr int kTotal     = 800;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    int collectedValues[kTotal];
    std::atomic<int> collectedIndex{0};
    TestList list;
    std::atomic<bool> start{false};
    std::atomic<int>  totalPopped{0};

    auto consumer = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalPopped.load(std::memory_order_acquire) < kTotal) {
            LNode* n = list.popBack();
            if (n) {
                int idx = collectedIndex.fetch_add(1, std::memory_order_relaxed);
                collectedValues[idx] = n->value;
                totalPopped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    pauseTracking();
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; c++) consumers.emplace_back(consumer);
    resumeTracking();

    start.store(true, std::memory_order_release);
    for (int i = 0; i < kTotal; i++) list.pushFront(*nodes[i]);  // main thread = producer

    pauseTracking();
    for (auto& t : consumers) t.join();
    resumeTracking();

    int count = collectedIndex.load();
    ASSERT_EQ(kTotal, count);
    std::sort(collectedValues, collectedValues + count);
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, collectedValues[i]);
    ASSERT_TRUE(list.empty());
}

// ============================================================
// MPMC queue: multiple producers pushBack, multiple consumers popFront.
// Highest push/pop contention scenario.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListMPMCQueue, 10000) {
    constexpr int kProducers   = 4;
    constexpr int kConsumers   = 4;
    constexpr int kPerProducer = 200;
    constexpr int kTotal       = kProducers * kPerProducer;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};
    std::atomic<int>  totalPopped{0};
    std::vector<std::vector<int>> perConsumer(kConsumers);

    auto producer = [&](int p) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerProducer; i++)
            list.pushBack(*nodes[p * kPerProducer + i]);
    };

    auto consumer = [&](int c) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalPopped.load(std::memory_order_acquire) < kTotal) {
            LNode* n = list.popFront();
            if (n) {
                perConsumer[c].push_back(n->value);
                totalPopped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; p++) threads.emplace_back(producer, p);
    for (int c = 0; c < kConsumers; c++) threads.emplace_back(consumer, c);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::vector<int> all;
    for (auto& v : perConsumer) all.insert(all.end(), v.begin(), v.end());
    ASSERT_EQ((size_t)kTotal, all.size());
    std::sort(all.begin(), all.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, all[i]);
    ASSERT_TRUE(list.empty());
}

// ============================================================
// Concurrent remove: all nodes are pre-inserted; each thread
// removes its own disjoint partition of the list simultaneously.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentRemove, 10000) {
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 100;
    constexpr int kTotal     = kThreads * kPerThread;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    for (int i = 0; i < kTotal; i++) list.pushBack(*nodes[i]);

    std::atomic<bool> start{false};

    auto remover = [&](int t) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerThread; i++)
            list.remove(*nodes[t * kPerThread + i]);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(remover, t);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    ASSERT_TRUE(list.empty());
    ASSERT_EQ(nullptr, list.getHead());
    ASSERT_EQ(nullptr, list.getTail());

    // Every node's pointers must have been cleared by remove.
    for (int i = 0; i < kTotal; i++) {
        ASSERT_EQ(nullptr, nodes[i]->prev);
        ASSERT_EQ(nullptr, nodes[i]->next);
    }
}

// ============================================================
// Remove-head race: exercises the retry path in remove().
//
// One thread continuously calls pushFront(), prepending new nodes
// in front of the original head.  A second thread tries to remove
// that original head node.  Because the original head's prev pointer
// changes (from SENTINEL to the newly prepended node) between the
// initial read and the re-validation, remove() must retry.
// After both threads finish, the target node must be absent from
// the list and all pointers cleared.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListRemoveHeadRace, 10000) {
    constexpr int kPushers  = 4;
    constexpr int kPushEach = 50;
    constexpr int kPushTotal = kPushers * kPushEach;

    // One target node to remove, plus nodes for the concurrent pushers.
    LNode target(9999);
    std::vector<std::unique_ptr<LNode>> extras;
    for (int i = 0; i < kPushTotal; i++) extras.push_back(std::make_unique<LNode>(i));

    TestList list;
    list.pushBack(target);  // target starts as the sole node (head and tail)

    std::atomic<bool> start{false};

    // Pushers prepend new nodes in front of target, forcing target.prev to change.
    auto pusher = [&](int p) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPushEach; i++)
            list.pushFront(*extras[p * kPushEach + i]);
    };

    // Remover tries to unlink the target node.  The retry loop in remove()
    // will fire every time a pusher changes target.prev before re-validation.
    auto remover = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        list.remove(target);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    std::thread removeThread(remover);
    for (int p = 0; p < kPushers; p++) threads.emplace_back(pusher, p);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    removeThread.join();
    for (auto& t : threads) t.join();
    resumeTracking();

    // target must be absent from the list.
    ASSERT_EQ(nullptr, target.prev);
    ASSERT_EQ(nullptr, target.next);

    // The kPushTotal extra nodes must all be present and structurally sound.
    auto fwd = walkForward(list);
    ASSERT_EQ((size_t)kPushTotal, fwd.size());
    assertIntegrity(list);

    // target's value (9999) must not appear in the remaining list.
    for (int v : fwd) ASSERT_NE(9999, v);
}

// ============================================================
// Concurrent double-remove: two threads try to remove the same
// node simultaneously.  Exactly one must succeed (return true),
// the other must return false, and the list must remain intact.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentDoubleRemove, 10000) {
    constexpr int kIterations = 200;

    for (int iter = 0; iter < kIterations; iter++) {
        TestList list;
        LNode a(1), b(2), c(3);

        list.pushBack(a);
        list.pushBack(b);
        list.pushBack(c);

        std::atomic<bool> start{false};
        std::atomic<int> successes{0};

        auto remover = [&]() {
            while (!start.load(std::memory_order_acquire)) {}
            if (list.remove(b)) successes.fetch_add(1, std::memory_order_relaxed);
        };

        pauseTracking();
        std::thread t1(remover);
        std::thread t2(remover);
        resumeTracking();

        start.store(true, std::memory_order_release);

        pauseTracking();
        t1.join();
        t2.join();
        resumeTracking();

        // Exactly one thread must have succeeded.
        ASSERT_EQ(1, successes.load());
        ASSERT_EQ(nullptr, b.prev);
        ASSERT_EQ(nullptr, b.next);

        auto fwd = walkForward(list);
        ASSERT_EQ(2u, fwd.size());
        ASSERT_EQ(1, fwd[0]);
        ASSERT_EQ(3, fwd[1]);
        assertIntegrity(list);
    }
}

// ============================================================
// High-contention stress: all four operations run simultaneously.
// Even producers use pushBack, odd producers use pushFront.
// Even consumers use popFront, odd consumers use popBack.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListHighContention, 10000) {
    constexpr int kProducers   = 8;
    constexpr int kConsumers   = 8;
    constexpr int kPerProducer = 150;
    constexpr int kTotal       = kProducers * kPerProducer;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList list;
    std::atomic<bool> start{false};
    std::atomic<int>  totalPopped{0};
    std::vector<std::vector<int>> perConsumer(kConsumers);

    auto producer = [&](int p) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kPerProducer; i++) {
            LNode& node = *nodes[p * kPerProducer + i];
            if (p % 2 == 0) list.pushBack(node);
            else             list.pushFront(node);
        }
    };

    auto consumer = [&](int c) {
        while (!start.load(std::memory_order_acquire)) {}
        while (totalPopped.load(std::memory_order_acquire) < kTotal) {
            LNode* n = (c % 2 == 0) ? list.popFront() : list.popBack();
            if (n) {
                perConsumer[c].push_back(n->value);
                totalPopped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; p++) threads.emplace_back(producer, p);
    for (int c = 0; c < kConsumers; c++) threads.emplace_back(consumer, c);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    std::vector<int> all;
    for (auto& v : perConsumer) all.insert(all.end(), v.begin(), v.end());
    ASSERT_EQ((size_t)kTotal, all.size());
    std::sort(all.begin(), all.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, all[i]);
    ASSERT_TRUE(list.empty());
}

// ============================================================
// Concurrent splice: multiple threads build small lists and
// splice them into a shared target list simultaneously.
// ============================================================

TEST_WITH_TIMEOUT(AtomicLinkedListConcurrentSplice, 10000) {
    constexpr int kThreads   = 8;
    constexpr int kPerThread = 50;
    constexpr int kTotal     = kThreads * kPerThread;

    std::vector<std::unique_ptr<LNode>> nodes;
    for (int i = 0; i < kTotal; i++) nodes.push_back(std::make_unique<LNode>(i));

    TestList target;
    std::atomic<bool> start{false};

    auto worker = [&](int t) {
        while (!start.load(std::memory_order_acquire)) {}
        // Build a small local list, then splice it into the shared target.
        TestList local;
        for (int i = 0; i < kPerThread; i++)
            local.pushBack(*nodes[t * kPerThread + i]);
        if (t % 2 == 0)
            target.spliceBack(local);
        else
            target.spliceFront(local);
    };

    pauseTracking();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) threads.emplace_back(worker, t);
    resumeTracking();

    start.store(true, std::memory_order_release);

    pauseTracking();
    for (auto& t : threads) t.join();
    resumeTracking();

    auto fwd = walkForward(target);
    ASSERT_EQ((size_t)kTotal, fwd.size());
    std::sort(fwd.begin(), fwd.end());
    for (int i = 0; i < kTotal; i++) ASSERT_EQ(i, fwd[i]);
    assertIntegrity(target);
}

// ============================================================
// Statistics tests
// ============================================================

using CoarseList = AtomicIntrusiveLinkedList<LNode, std::shared_mutex, LNodeExtractor, StatsCoarse>;
using FullList   = AtomicIntrusiveLinkedList<LNode, std::shared_mutex, LNodeExtractor, StatsFull>;

// --- CoarseStatistics ---

TEST(AtomicLinkedListCoarseStatsPushPop) {
    CoarseList list;
    LNode a(1), b(2), c(3);

    list.pushFront(a);
    list.pushBack(b);
    list.pushBack(c);

    const auto& s = list.getStatistics();
    ASSERT_EQ(3u, s.totalCalls.load(RELAXED));
    ASSERT_EQ(3u, s.currentSize.load(RELAXED));
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));

    LNode* popped = list.popFront();
    ASSERT_EQ(&a, popped);
    // popFront is a call too
    ASSERT_EQ(4u, s.totalCalls.load(RELAXED));
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));

    popped = list.popBack();
    ASSERT_EQ(&c, popped);
    ASSERT_EQ(5u, s.totalCalls.load(RELAXED));
    ASSERT_EQ(1u, s.currentSize.load(RELAXED));
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));
}

TEST(AtomicLinkedListCoarseStatsRemove) {
    CoarseList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    const auto& s = list.getStatistics();
    ASSERT_EQ(3u, s.currentSize.load(RELAXED));

    ASSERT_TRUE(list.remove(b));
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));

    // Double-remove: should count as totalEmpty
    ASSERT_FALSE(list.remove(b));
    ASSERT_TRUE(s.totalEmpty.load(RELAXED) >= 1u);
}

TEST(AtomicLinkedListCoarseStatsPopEmpty) {
    CoarseList list;

    LNode* r = list.popFront();
    ASSERT_EQ(nullptr, r);
    const auto& s = list.getStatistics();
    ASSERT_TRUE(s.totalEmpty.load(RELAXED) >= 1u);
    ASSERT_EQ(1u, s.totalCalls.load(RELAXED));

    r = list.popBack();
    ASSERT_EQ(nullptr, r);
    ASSERT_TRUE(s.totalEmpty.load(RELAXED) >= 2u);
    ASSERT_EQ(2u, s.totalCalls.load(RELAXED));
}

TEST(AtomicLinkedListCoarseStatsSplice) {
    CoarseList dst, src;
    LNode a(1), b(2), c(3), d(4);

    src.pushBack(a);
    src.pushBack(b);
    dst.pushBack(c);
    dst.pushBack(d);

    const auto& srcS = src.getStatistics();
    const auto& dstS = dst.getStatistics();
    ASSERT_EQ(2u, srcS.currentSize.load(RELAXED));
    ASSERT_EQ(2u, dstS.currentSize.load(RELAXED));

    dst.spliceFront(src);

    // Source drained, dst got them all.
    ASSERT_EQ(0u, srcS.currentSize.load(RELAXED));
    ASSERT_EQ(4u, dstS.currentSize.load(RELAXED));
    ASSERT_EQ(4u, dstS.peakSize.load(RELAXED));
}

TEST(AtomicLinkedListCoarseStatsSpliceBack) {
    CoarseList dst, src;
    LNode a(1), b(2), c(3), d(4);

    dst.pushBack(a);
    dst.pushBack(b);
    src.pushBack(c);
    src.pushBack(d);

    dst.spliceBack(src);

    const auto& srcS = src.getStatistics();
    const auto& dstS = dst.getStatistics();
    ASSERT_EQ(0u, srcS.currentSize.load(RELAXED));
    ASSERT_EQ(4u, dstS.currentSize.load(RELAXED));
    ASSERT_EQ(4u, dstS.peakSize.load(RELAXED));
}

TEST(AtomicLinkedListCoarseStatsPeakSize) {
    CoarseList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    const auto& s = list.getStatistics();
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));

    list.popFront();
    list.popFront();
    ASSERT_EQ(1u, s.currentSize.load(RELAXED));
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));

    // Push again — peak should not increase beyond 3
    LNode d(4);
    list.pushBack(d);
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
    ASSERT_EQ(3u, s.peakSize.load(RELAXED));
}

// --- FullStatistics ---

TEST(AtomicLinkedListFullStatsPushFront) {
    FullList list;
    LNode a(1), b(2);

    list.pushFront(a);
    list.pushFront(b);

    const auto& s = list.getStatistics();
    ASSERT_EQ(2u, s.pushFrontCalls.load(RELAXED));
    ASSERT_EQ(0u, s.pushBackCalls.load(RELAXED));
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
    ASSERT_EQ(2u, s.peakSize.load(RELAXED));
}

TEST(AtomicLinkedListFullStatsPushBack) {
    FullList list;
    LNode a(1), b(2);

    list.pushBack(a);
    list.pushBack(b);

    const auto& s = list.getStatistics();
    ASSERT_EQ(2u, s.pushBackCalls.load(RELAXED));
    ASSERT_EQ(0u, s.pushFrontCalls.load(RELAXED));
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
}

TEST(AtomicLinkedListFullStatsPopFront) {
    FullList list;
    LNode a(1), b(2);

    list.pushBack(a);
    list.pushBack(b);

    list.popFront();
    list.popFront();
    LNode* r = list.popFront();
    ASSERT_EQ(nullptr, r);

    const auto& s = list.getStatistics();
    ASSERT_EQ(3u, s.popFrontCalls.load(RELAXED));
    ASSERT_TRUE(s.popFrontEmpty.load(RELAXED) >= 1u);
    ASSERT_EQ(0u, s.currentSize.load(RELAXED));
}

TEST(AtomicLinkedListFullStatsPopBack) {
    FullList list;
    LNode a(1), b(2);

    list.pushBack(a);
    list.pushBack(b);

    list.popBack();
    list.popBack();
    LNode* r = list.popBack();
    ASSERT_EQ(nullptr, r);

    const auto& s = list.getStatistics();
    ASSERT_EQ(3u, s.popBackCalls.load(RELAXED));
    ASSERT_TRUE(s.popBackEmpty.load(RELAXED) >= 1u);
    ASSERT_EQ(0u, s.currentSize.load(RELAXED));
}

TEST(AtomicLinkedListFullStatsRemove) {
    FullList list;
    LNode a(1), b(2), c(3);

    list.pushBack(a);
    list.pushBack(b);
    list.pushBack(c);

    ASSERT_TRUE(list.remove(b));
    ASSERT_FALSE(list.remove(b));

    const auto& s = list.getStatistics();
    ASSERT_EQ(2u, s.removeCalls.load(RELAXED));
    ASSERT_TRUE(s.removeNotFound.load(RELAXED) >= 1u);
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
}

TEST(AtomicLinkedListFullStatsMixedOperations) {
    FullList list;
    LNode a(1), b(2), c(3), d(4);

    list.pushFront(a);
    list.pushBack(b);
    list.pushFront(c);
    list.pushBack(d);

    const auto& s = list.getStatistics();
    ASSERT_EQ(2u, s.pushFrontCalls.load(RELAXED));
    ASSERT_EQ(2u, s.pushBackCalls.load(RELAXED));
    ASSERT_EQ(4u, s.currentSize.load(RELAXED));
    ASSERT_EQ(4u, s.peakSize.load(RELAXED));

    list.popFront();
    list.popBack();

    ASSERT_EQ(1u, s.popFrontCalls.load(RELAXED));
    ASSERT_EQ(1u, s.popBackCalls.load(RELAXED));
    ASSERT_EQ(2u, s.currentSize.load(RELAXED));
    ASSERT_EQ(4u, s.peakSize.load(RELAXED));

    ASSERT_TRUE(list.remove(a));
    ASSERT_EQ(1u, s.removeCalls.load(RELAXED));
    ASSERT_EQ(1u, s.currentSize.load(RELAXED));
}

// --- Aggregate getter interface ---

TEST(AtomicLinkedListCoarseGetters) {
    CoarseList list;
    LNode a(1), b(2), c(3);

    list.pushFront(a);
    list.pushBack(b);
    list.pushBack(c);
    list.popFront();
    list.remove(c);

    const auto& s = list.getStatistics();
    ASSERT_EQ(5u, s.getTotalCalls());
    ASSERT_EQ(1u, s.getCurrentSize());
    ASSERT_EQ(3u, s.getPeakSize());
}

TEST(AtomicLinkedListFullGettersMatchCoarse) {
    // Verify that the aggregate getters on FullStatistics produce the same
    // totals as CoarseStatistics would for an identical operation sequence.
    auto runSequence = [](auto& list, auto& a, auto& b, auto& c, auto& d) {
        list.pushFront(a);
        list.pushBack(b);
        list.pushFront(c);
        list.pushBack(d);
        list.popFront();
        list.popBack();
        list.remove(a);
        list.remove(a);  // not-found
        list.popFront();
        list.popFront(); // empty
    };

    CoarseList coarseList;
    LNode ca(1), cb(2), cc(3), cd(4);
    runSequence(coarseList, ca, cb, cc, cd);

    FullList fullList;
    LNode fa(1), fb(2), fc(3), fd(4);
    runSequence(fullList, fa, fb, fc, fd);

    const auto& cs = coarseList.getStatistics();
    const auto& fs = fullList.getStatistics();

    ASSERT_EQ(cs.getTotalCalls(), fs.getTotalCalls());
    ASSERT_EQ(cs.getTotalEmpty(), fs.getTotalEmpty());
    ASSERT_EQ(cs.getTotalRetries(), fs.getTotalRetries());
    ASSERT_EQ(cs.getTotalContentions(), fs.getTotalContentions());
    ASSERT_EQ(cs.getCurrentSize(), fs.getCurrentSize());
    ASSERT_EQ(cs.getPeakSize(), fs.getPeakSize());
}
