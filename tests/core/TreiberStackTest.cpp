//
// Unit + concurrent stress tests for Core::TreiberStack and
// Core::ChainedTreiberStack (vmsmalloc Phase 4).
// Created by Spencer Martin on 5/28/26.
//
// Coverage map (see specs/vmsmalloc-phase-4.md Verification Targets / Testing
// Approach for the authoritative list):
//
//   - Static asserts: Uint128HeadEncoding pack/unpack round-trip, zero-init
//     empty, sizeof==16, __atomic_is_lock_free(16) on host, intrinsic atomic
//     ops cover sizeof==16.
//   - TreiberStack: single-thread push/pop LIFO ordering, pop-empty, peek,
//     concurrent producer/consumer multiset balance.
//   - ChainedTreiberStack: empty pop returns {nullptr,0}; push with
//     maxChainLength=1 produces singletons; push extends to maxChainLength
//     then starts a new chain; setMaxChainLength affects future pushes only;
//     pushChain publishes a pre-built chain as a new top; pushChain ignores
//     maxChainLength; pushChain + push(element) interop extends the
//     pushChain'd chain; concurrent push/pushChain/pop stress with multiset
//     correctness.
//   - Hooks contract: NoopHooks is zero-overhead; a counting Hooks instance
//     fires the expected counts under controlled scenarios.
//

#include "../test.h"
#include <harness/TestHarness.h>
#include <core/atomic/TreiberStack.h>

#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <algorithm>

using Core::TreiberStack;
using Core::ChainedTreiberStack;
using Core::Uint128HeadEncoding;
using Core::NoopHooks;

// ============================================================
// Test node + extractors
//
// next / chainNext are Atomic<T*> with RELAXED ops — per parent-spec DEC-042
// #3, the Atomic<> wrapper itself (not the ordering) is load-bearing for C++
// data-race-freedom in Treiber-stack consumers. vmsmalloc's SlabDescriptor
// uses the same shape (Atomic<SlabDescriptor*> chainNext). The synchronizes-
// with edge comes from the Treiber CAS (push RELEASE / pop ACQUIRE); RELAXED
// on the linkage fields is sufficient.
// chainDepth: uint32_t. It's only written by the producer of the new chain
// head (before the publishing CAS) and read by consumers post-pop / by push-
// extend (synchronized via the same CAS edge). The spec does not require
// Atomic on chainDepth, but for the test we make it Atomic<uint32_t> to keep
// TSan happy in scenarios where a node may be re-pushed while another thread
// has already read the head pointer but not yet completed its CAS.
// ============================================================

struct TestNode {
    Atomic<TestNode*> next{nullptr};         // shared-stack linkage
    Atomic<TestNode*> chainNext{nullptr};    // intra-chain linkage (ChainedTreiberStack)
    Atomic<uint32_t>  chainDepth{0};         // intra-chain depth field (head only)
    int               payload = 0;           // for ordering / multiset verification
};

struct TestNodeLinkage {
    static TestNode* getNext(TestNode& n) {
        return n.next.load(RELAXED);
    }
    static void setNext(TestNode& n, TestNode* p) {
        n.next.store(p, RELAXED);
    }
};

struct TestNodeChainLinkage {
    static TestNode* getChainNext(TestNode& n) {
        return n.chainNext.load(RELAXED);
    }
    static void setChainNext(TestNode& n, TestNode* p) {
        n.chainNext.store(p, RELAXED);
    }
    static uint32_t getChainDepth(TestNode& n) {
        return n.chainDepth.load(RELAXED);
    }
    static void setChainDepth(TestNode& n, uint32_t d) {
        n.chainDepth.store(d, RELAXED);
    }
};

using DefaultEncoding = Uint128HeadEncoding<TestNode>;

// ============================================================
// Static-assert block: encoding contract + Atomic<16-byte> intrinsic path
// ============================================================

static_assert(sizeof(DefaultEncoding::Storage) == 16,
              "Uint128HeadEncoding::Storage must be exactly 16 bytes");
static_assert(__atomic_is_lock_free(16, nullptr),
              "Host target must support lock-free 16-byte CAS for these tests");
static_assert(_use_intrinsic_atomic_ops<DefaultEncoding::Storage>,
              "Atomic<Uint128HeadEncoding::Storage> must use the intrinsic path");

// Zero-init empty invariant: unpacking a zero Storage yields nullptr. The
// reinterpret_cast in unpackPointer prevents a constexpr static_assert, but
// we can verify the byte representation: a zero-initialized Storage has
// ptr==0, which by reinterpret_cast contract round-trips to nullptr.
static_assert(DefaultEncoding::Storage{}.ptr == 0,
              "Zero-initialized Storage must have ptr == 0");
static_assert(DefaultEncoding::Storage{}.tag == 0,
              "Zero-initialized Storage must have tag == 0");
static_assert(DefaultEncoding::advanceTag(0) == 1,
              "advanceTag(0) must be 1");
static_assert(DefaultEncoding::advanceTag(uint64_t{42}) == 43,
              "advanceTag(42) must be 43");

// Runtime probe at namespace scope for the round-trip property (constexpr
// can't traverse reinterpret_cast).
static const bool kZeroStorageUnpacksToNullCheck = [] {
    DefaultEncoding::Storage zero{};
    return DefaultEncoding::unpackPointer(zero) == nullptr
        && DefaultEncoding::unpackTag(zero) == 0;
}();

// ============================================================
// Counting hooks: used to verify hook fire counts.
// ============================================================

struct CountingHooks {
    std::atomic<uint64_t>* preTouchCount = nullptr;
    std::atomic<uint64_t>* casFailureCount = nullptr;
    std::atomic<uint64_t>* emptyStackCount = nullptr;

    template <typename T> void onPreTouch(T*) const noexcept {
        if (preTouchCount) preTouchCount->fetch_add(1, std::memory_order_relaxed);
    }
    void onCasFailure() const noexcept {
        if (casFailureCount) casFailureCount->fetch_add(1, std::memory_order_relaxed);
    }
    void onEmptyStack() const noexcept {
        if (emptyStackCount) emptyStackCount->fetch_add(1, std::memory_order_relaxed);
    }
};

// ============================================================
// TreiberStack — single-threaded unit tests
// ============================================================

TEST(Uint128HeadEncoding_ZeroStorageUnpacksToNullAndZero) {
    ASSERT_TRUE(kZeroStorageUnpacksToNullCheck);
    DefaultEncoding::Storage zero{};
    ASSERT_EQ(DefaultEncoding::unpackPointer(zero), static_cast<TestNode*>(nullptr));
    ASSERT_EQ(DefaultEncoding::unpackTag(zero), uint64_t(0));
}

TEST(Uint128HeadEncoding_PackUnpackRoundTrip) {
    TestNode dummy;
    DefaultEncoding::Storage s = DefaultEncoding::pack(&dummy, uint64_t{0xABCDEF12});
    ASSERT_EQ(DefaultEncoding::unpackPointer(s), &dummy);
    ASSERT_EQ(DefaultEncoding::unpackTag(s), uint64_t(0xABCDEF12));
    ASSERT_EQ(DefaultEncoding::advanceTag(DefaultEncoding::unpackTag(s)),
              uint64_t(0xABCDEF13));
}

TEST(TreiberStack_EmptyPopReturnsNull) {
    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;
    ASSERT_TRUE(stack.empty());
    ASSERT_EQ(stack.peek(), static_cast<TestNode*>(nullptr));
    ASSERT_EQ(stack.pop(), static_cast<TestNode*>(nullptr));
}

TEST(TreiberStack_PushPopLifoOrder) {
    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;
    std::vector<TestNode> nodes(8);
    for (size_t i = 0; i < nodes.size(); i++) {
        nodes[i].payload = static_cast<int>(i);
        stack.push(nodes[i]);
    }
    ASSERT_FALSE(stack.empty());
    // Expect LIFO: 7, 6, 5, ..., 0.
    for (int expected = 7; expected >= 0; expected--) {
        TestNode* n = stack.pop();
        ASSERT_NE(n, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(n->payload, expected);
    }
    ASSERT_TRUE(stack.empty());
    ASSERT_EQ(stack.pop(), static_cast<TestNode*>(nullptr));
}

TEST(TreiberStack_PeekDoesNotConsume) {
    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;
    TestNode a, b;
    a.payload = 1;
    b.payload = 2;
    stack.push(a);
    stack.push(b);
    ASSERT_EQ(stack.peek()->payload, 2);
    ASSERT_EQ(stack.peek()->payload, 2);
    TestNode* popped = stack.pop();
    ASSERT_EQ(popped->payload, 2);
    ASSERT_EQ(stack.peek()->payload, 1);
}

// ============================================================
// ChainedTreiberStack — single-threaded unit tests
// ============================================================

using ChainStack = ChainedTreiberStack<
    TestNode, TestNodeLinkage, TestNodeChainLinkage, DefaultEncoding>;

TEST(ChainedTreiberStack_EmptyPopReturnsZeros) {
    ChainStack stack(8);
    ASSERT_TRUE(stack.empty());
    auto p = stack.pop();
    ASSERT_EQ(p.head, static_cast<TestNode*>(nullptr));
    ASSERT_EQ(p.depth, uint32_t(0));
}

TEST(ChainedTreiberStack_MaxLen1_ProducesSingletons) {
    ChainStack stack(1);
    std::vector<TestNode> nodes(5);
    for (size_t i = 0; i < nodes.size(); i++) {
        nodes[i].payload = static_cast<int>(i);
        stack.push(nodes[i]);
    }
    // All five chains are singletons because maxChainLength==1.
    for (int expected = 4; expected >= 0; expected--) {
        auto p = stack.pop();
        ASSERT_NE(p.head, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(p.depth, uint32_t(1));
        ASSERT_EQ(p.head->payload, expected);
        ASSERT_EQ(p.head->chainNext.load(RELAXED), static_cast<TestNode*>(nullptr));
    }
    ASSERT_TRUE(stack.empty());
}

TEST(ChainedTreiberStack_PushExtendsThenStartsNew) {
    // maxChainLength = 4; push 10 elements; expect chains of depth 4, 4, 2 (newest-first).
    ChainStack stack(4);
    std::vector<TestNode> nodes(10);
    for (size_t i = 0; i < nodes.size(); i++) {
        nodes[i].payload = static_cast<int>(i);
        stack.push(nodes[i]);
    }
    // First pop: top chain, which contains elements 9, 8 (depth 2).
    auto p0 = stack.pop();
    ASSERT_EQ(p0.depth, uint32_t(2));
    ASSERT_EQ(p0.head->payload, 9);
    ASSERT_EQ(p0.head->chainNext.load(RELAXED)->payload, 8);
    ASSERT_EQ(p0.head->chainNext.load(RELAXED)->chainNext.load(RELAXED),
              static_cast<TestNode*>(nullptr));

    // Second pop: elements 7,6,5,4 (depth 4).
    auto p1 = stack.pop();
    ASSERT_EQ(p1.depth, uint32_t(4));
    int expected1[] = { 7, 6, 5, 4 };
    TestNode* walk = p1.head;
    for (int e : expected1) {
        ASSERT_NE(walk, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(walk->payload, e);
        walk = walk->chainNext.load(RELAXED);
    }
    ASSERT_EQ(walk, static_cast<TestNode*>(nullptr));

    // Third pop: elements 3,2,1,0 (depth 4).
    auto p2 = stack.pop();
    ASSERT_EQ(p2.depth, uint32_t(4));
    int expected2[] = { 3, 2, 1, 0 };
    walk = p2.head;
    for (int e : expected2) {
        ASSERT_NE(walk, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(walk->payload, e);
        walk = walk->chainNext.load(RELAXED);
    }
    ASSERT_EQ(walk, static_cast<TestNode*>(nullptr));

    ASSERT_TRUE(stack.empty());
}

TEST(ChainedTreiberStack_SetMaxChainLengthAffectsFuturePushes) {
    ChainStack stack(4);
    std::vector<TestNode> nodes(8);
    for (size_t i = 0; i < 4; i++) {
        nodes[i].payload = static_cast<int>(i);
        stack.push(nodes[i]);
    }
    // Top chain is now depth-4 (max). Dropping max to 2 must not truncate it,
    // but the next push must start a NEW singleton (not extend).
    stack.setMaxChainLength(2);
    ASSERT_EQ(stack.getMaxChainLength(), uint32_t(2));

    nodes[4].payload = 4;
    stack.push(nodes[4]);

    // Newest pop: singleton {4}.
    auto p0 = stack.pop();
    ASSERT_EQ(p0.depth, uint32_t(1));
    ASSERT_EQ(p0.head->payload, 4);

    // Next pop: the OLD depth-4 chain stays at depth 4 (no truncation).
    auto p1 = stack.pop();
    ASSERT_EQ(p1.depth, uint32_t(4));
}

TEST(ChainedTreiberStack_ShrinkMaxDoesNotTruncateTop) {
    ChainStack stack(4);
    std::vector<TestNode> nodes(4);
    for (size_t i = 0; i < 4; i++) {
        nodes[i].payload = static_cast<int>(i);
        stack.push(nodes[i]);
    }
    stack.setMaxChainLength(1);
    auto pk = stack.peek();
    ASSERT_EQ(pk.depth, uint32_t(4));
    auto p = stack.pop();
    ASSERT_EQ(p.depth, uint32_t(4));
}

TEST(ChainedTreiberStack_PushChainPublishesPrebuilt) {
    ChainStack stack(8);
    // Build a 5-element chain locally: head=nodes[0], chainNext walks
    // nodes[0]->nodes[1]->...->nodes[4]; nodes[4].chainNext == nullptr.
    std::vector<TestNode> nodes(5);
    for (size_t i = 0; i < nodes.size(); i++) nodes[i].payload = static_cast<int>(i);
    for (size_t i = 0; i + 1 < nodes.size(); i++) nodes[i].chainNext = &nodes[i + 1];
    nodes.back().chainNext = nullptr;
    nodes[0].chainDepth = 5;

    stack.pushChain(nodes[0], 5);
    auto p = stack.pop();
    ASSERT_EQ(p.depth, uint32_t(5));
    ASSERT_EQ(p.head, &nodes[0]);

    TestNode* walk = p.head;
    for (int i = 0; i < 5; i++) {
        ASSERT_NE(walk, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(walk->payload, i);
        walk = walk->chainNext.load(RELAXED);
    }
    ASSERT_EQ(walk, static_cast<TestNode*>(nullptr));
    ASSERT_TRUE(stack.empty());
}

TEST(ChainedTreiberStack_PushChainDoesNotExtend) {
    // Push three single elements (top chain becomes depth 3).
    // Then pushChain a depth-4 chain.
    // Pop must return the depth-4 chain first (not depth-7).
    ChainStack stack(16);

    std::vector<TestNode> singles(3);
    for (size_t i = 0; i < singles.size(); i++) {
        singles[i].payload = static_cast<int>(100 + i);
        stack.push(singles[i]);
    }
    auto peeked = stack.peek();
    ASSERT_EQ(peeked.depth, uint32_t(3));

    std::vector<TestNode> chain(4);
    for (size_t i = 0; i < chain.size(); i++) chain[i].payload = static_cast<int>(200 + i);
    for (size_t i = 0; i + 1 < chain.size(); i++) chain[i].chainNext = &chain[i + 1];
    chain.back().chainNext = nullptr;
    chain[0].chainDepth = 4;
    stack.pushChain(chain[0], 4);

    auto p0 = stack.pop();
    ASSERT_EQ(p0.depth, uint32_t(4));
    ASSERT_EQ(p0.head, &chain[0]);

    auto p1 = stack.pop();
    ASSERT_EQ(p1.depth, uint32_t(3));
    ASSERT_EQ(p1.head, &singles[2]);
}

TEST(ChainedTreiberStack_PushChainIgnoresMaxChainLength) {
    ChainStack stack(2);   // very small max
    std::vector<TestNode> chain(10);
    for (size_t i = 0; i < chain.size(); i++) chain[i].payload = static_cast<int>(i);
    for (size_t i = 0; i + 1 < chain.size(); i++) chain[i].chainNext = &chain[i + 1];
    chain.back().chainNext = nullptr;
    chain[0].chainDepth = 10;

    stack.pushChain(chain[0], 10);
    auto p = stack.pop();
    ASSERT_EQ(p.depth, uint32_t(10));
    ASSERT_EQ(p.head, &chain[0]);
}

TEST(ChainedTreiberStack_PushChainThenPushElementExtends) {
    // pushChain a depth-4 chain, then push(element) with maxChainLength=8.
    // Pop should return depth-5 with the new element on top.
    ChainStack stack(8);
    std::vector<TestNode> chain(4);
    for (size_t i = 0; i < chain.size(); i++) chain[i].payload = static_cast<int>(100 + i);
    for (size_t i = 0; i + 1 < chain.size(); i++) chain[i].chainNext = &chain[i + 1];
    chain.back().chainNext = nullptr;
    chain[0].chainDepth = 4;
    stack.pushChain(chain[0], 4);

    TestNode extra;
    extra.payload = 999;
    stack.push(extra);

    auto p = stack.pop();
    ASSERT_EQ(p.depth, uint32_t(5));
    ASSERT_EQ(p.head, &extra);
    ASSERT_EQ(p.head->chainNext.load(RELAXED), &chain[0]);
    int expected[] = { 999, 100, 101, 102, 103 };
    TestNode* walk = p.head;
    for (int e : expected) {
        ASSERT_NE(walk, static_cast<TestNode*>(nullptr));
        ASSERT_EQ(walk->payload, e);
        walk = walk->chainNext.load(RELAXED);
    }
    ASSERT_EQ(walk, static_cast<TestNode*>(nullptr));
}

// ============================================================
// Hooks tests
// ============================================================

TEST(ChainedTreiberStack_NoopHooksZeroOverhead) {
    // NoopHooks is empty; [[no_unique_address]] should ensure the chained
    // stack's footprint matches the layout of head + maxChainLength + padding.
    // We cannot assert exact sizeof (it depends on cache-line padding), but
    // we can assert NoopHooks itself is empty/zero-size after the empty-base
    // optimization.
    ASSERT_TRUE(__is_empty(NoopHooks));
}

TEST(ChainedTreiberStack_CountingHooksOnPreTouchAndOnEmptyStack) {
    std::atomic<uint64_t> preTouch{0};
    std::atomic<uint64_t> casFail{0};
    std::atomic<uint64_t> empty{0};
    CountingHooks h{ &preTouch, &casFail, &empty };
    using HookedStack = ChainedTreiberStack<
        TestNode, TestNodeLinkage, TestNodeChainLinkage, DefaultEncoding,
        CountingHooks>;
    HookedStack stack(4, h);

    // Empty pop fires onEmptyStack exactly once.
    auto p = stack.pop();
    ASSERT_EQ(p.head, static_cast<TestNode*>(nullptr));
    ASSERT_EQ(empty.load(), uint64_t(1));
    ASSERT_EQ(preTouch.load(), uint64_t(0));
    ASSERT_EQ(casFail.load(), uint64_t(0));

    // push onto empty stack: no onPreTouch (we never read *topPtr when top is null).
    TestNode a;
    a.payload = 1;
    stack.push(a);
    ASSERT_EQ(preTouch.load(), uint64_t(0));

    // push onto non-empty stack with depth < max: extend path reads *topPtr,
    // so onPreTouch fires exactly once (assuming no CAS contention in single-threaded).
    TestNode b;
    b.payload = 2;
    stack.push(b);
    ASSERT_EQ(preTouch.load(), uint64_t(1));

    // pop: also fires onPreTouch (we read *topPtr inside pop).
    auto pp = stack.pop();
    ASSERT_EQ(pp.depth, uint32_t(2));
    ASSERT_EQ(preTouch.load(), uint64_t(2));

    // No CAS contention in single-threaded — failure count remains zero.
    ASSERT_EQ(casFail.load(), uint64_t(0));
}

TEST(ChainedTreiberStack_PushChainDoesNotFirePreTouch) {
    std::atomic<uint64_t> preTouch{0};
    std::atomic<uint64_t> casFail{0};
    std::atomic<uint64_t> empty{0};
    CountingHooks h{ &preTouch, &casFail, &empty };
    using HookedStack = ChainedTreiberStack<
        TestNode, TestNodeLinkage, TestNodeChainLinkage, DefaultEncoding,
        CountingHooks>;
    HookedStack stack(8, h);

    // Even with a non-empty top, pushChain doesn't read *oldPtr's fields.
    TestNode seed;
    seed.payload = 1;
    stack.push(seed);
    uint64_t preTouchBefore = preTouch.load();

    std::vector<TestNode> chain(3);
    for (size_t i = 0; i < chain.size(); i++) chain[i].payload = static_cast<int>(10 + i);
    for (size_t i = 0; i + 1 < chain.size(); i++) chain[i].chainNext = &chain[i + 1];
    chain.back().chainNext = nullptr;
    chain[0].chainDepth = 3;
    stack.pushChain(chain[0], 3);

    ASSERT_EQ(preTouch.load(), preTouchBefore);  // unchanged
}

// ============================================================
// Concurrent stress — TreiberStack producer/consumer multiset balance
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(TreiberStack_Concurrent_ProducerConsumerMultiset, 10000) {
    constexpr size_t kProducers = 4;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerProducer = 10000;
    constexpr size_t kTotal = kProducers * kPerProducer;

    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;

    // Stable backing storage; pointers into this vector are pushed onto the stack.
    std::vector<TestNode> pool(kTotal);
    for (size_t i = 0; i < kTotal; i++) pool[i].payload = static_cast<int>(i);

    std::atomic<bool> startFlag{false};
    std::atomic<size_t> popped{0};

    // Each consumer collects payloads it observes; we'll merge to verify the
    // multiset of popped values matches the pushed multiset exactly.
    std::vector<std::vector<int>> consumerPayloads(kConsumers);

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; p++) {
        producers.emplace_back([&, p] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < kPerProducer; i++) {
                size_t idx = p * kPerProducer + i;
                stack.push(pool[idx]);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; c++) {
        consumers.emplace_back([&, c] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            std::vector<int>& mine = consumerPayloads[c];
            mine.reserve(kPerProducer);
            while (true) {
                TestNode* n = stack.pop();
                if (n) {
                    mine.push_back(n->payload);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else if (popped.load(std::memory_order_acquire) >= kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    startFlag.store(true, std::memory_order_release);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    ASSERT_EQ(popped.load(), kTotal);
    ASSERT_TRUE(stack.empty());

    // Multiset equality: collect all popped payloads, sort, compare with
    // [0..kTotal-1].
    std::vector<int> all;
    all.reserve(kTotal);
    for (auto& v : consumerPayloads) all.insert(all.end(), v.begin(), v.end());
    ASSERT_EQ(all.size(), kTotal);
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < kTotal; i++) ASSERT_EQ(all[i], static_cast<int>(i));
}

// ============================================================
// Concurrent stress — TreiberStack DEC-042 payload-visibility probe
// ============================================================
//
// Each pushed node carries a payload that the popper reads immediately after
// the pop. If pop's ACQUIRE-load is downgraded to RELAXED, ARMv8/TSan would
// report a race on the payload field. On x86 the TSO model masks the bug;
// this test is the ARMv8 regression sentinel.
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(TreiberStack_Concurrent_AcquireReleasePayloadVisibility, 10000) {
    constexpr size_t kProducers = 4;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerProducer = 8000;
    constexpr size_t kTotal = kProducers * kPerProducer;

    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;
    std::vector<TestNode> pool(kTotal);

    std::atomic<bool> startFlag{false};
    std::atomic<size_t> popped{0};
    std::atomic<bool> badPayload{false};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; p++) {
        producers.emplace_back([&, p] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < kPerProducer; i++) {
                size_t idx = p * kPerProducer + i;
                // Write payload before push; pop's ACQUIRE synchronizes-with
                // push's RELEASE, so the popper must see this payload.
                pool[idx].payload = static_cast<int>(idx ^ 0xDEADBEEF);
                stack.push(pool[idx]);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; c++) {
        consumers.emplace_back([&] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            while (true) {
                TestNode* n = stack.pop();
                if (n) {
                    size_t idx = static_cast<size_t>(n - &pool[0]);
                    int expected = static_cast<int>(idx ^ 0xDEADBEEF);
                    if (n->payload != expected) badPayload.store(true);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else if (popped.load() >= kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    startFlag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    ASSERT_EQ(popped.load(), kTotal);
    ASSERT_FALSE(badPayload.load());
}

// ============================================================
// Concurrent stress — ChainedTreiberStack chain-extend correctness
// ============================================================
//
// Producers push individual elements; consumers pop chains and walk them,
// verifying chainDepth matches the chainNext walk length and the bottom's
// chainNext is null.
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(ChainedTreiberStack_Concurrent_ChainExtendStress, 10000) {
    constexpr size_t kProducers = 4;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerProducer = 5000;
    constexpr size_t kTotal = kProducers * kPerProducer;
    constexpr uint32_t kMax = 16;

    ChainStack stack(kMax);
    std::vector<TestNode> pool(kTotal);
    for (size_t i = 0; i < kTotal; i++) pool[i].payload = static_cast<int>(i);

    std::atomic<bool> startFlag{false};
    std::atomic<size_t> popped{0};
    std::atomic<bool> badChain{false};

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kProducers; p++) {
        producers.emplace_back([&, p] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < kPerProducer; i++) {
                size_t idx = p * kPerProducer + i;
                stack.push(pool[idx]);
            }
        });
    }

    std::vector<std::vector<int>> consumerPayloads(kConsumers);
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; c++) {
        consumers.emplace_back([&, c] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            std::vector<int>& mine = consumerPayloads[c];
            while (true) {
                auto chain = stack.pop();
                if (chain.head) {
                    // Walk the chain, verify depth matches and bottom's
                    // chainNext is null.
                    uint32_t walked = 0;
                    TestNode* n = chain.head;
                    while (n) {
                        mine.push_back(n->payload);
                        walked++;
                        TestNode* next = n->chainNext.load(RELAXED);
                        if (next == nullptr) break;
                        n = next;
                    }
                    if (walked != chain.depth) badChain.store(true);
                    if (chain.depth == 0 || chain.depth > kMax) badChain.store(true);
                    popped.fetch_add(walked, std::memory_order_relaxed);
                } else if (popped.load() >= kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    startFlag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    ASSERT_EQ(popped.load(), kTotal);
    ASSERT_FALSE(badChain.load());
    ASSERT_TRUE(stack.empty());

    // Multiset equality.
    std::vector<int> all;
    all.reserve(kTotal);
    for (auto& v : consumerPayloads) all.insert(all.end(), v.begin(), v.end());
    ASSERT_EQ(all.size(), kTotal);
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < kTotal; i++) ASSERT_EQ(all[i], static_cast<int>(i));
}

// ============================================================
// Concurrent stress — mixed push / pushChain / pop
// ============================================================
//
// Some producers push individual elements (extend or singleton); others build
// local chains of size 8 and pushChain them. Consumers pop and walk.
// Verifies (a) every popped element accounted for exactly once,
// (b) chain depths consistent with the chainNext walk,
// (c) no element appears in two popped chains.
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(ChainedTreiberStack_Concurrent_MixedPushAndPushChain, 10000) {
    constexpr size_t kElemProducers = 3;
    constexpr size_t kChainProducers = 3;
    constexpr size_t kConsumers = 4;
    constexpr size_t kPerElemProducer = 4000;        // 12000 element-pushes
    constexpr size_t kPerChainProducer = 500;        // 500 chains of 8 = 12000 chain-pushes
    constexpr uint32_t kChainDepth = 8;
    constexpr size_t kTotalElem = kElemProducers * kPerElemProducer;
    constexpr size_t kTotalChain = kChainProducers * kPerChainProducer * kChainDepth;
    constexpr size_t kTotal = kTotalElem + kTotalChain;
    constexpr uint32_t kMax = 8;

    ChainStack stack(kMax);

    std::vector<TestNode> pool(kTotal);
    for (size_t i = 0; i < kTotal; i++) pool[i].payload = static_cast<int>(i);

    std::atomic<bool> startFlag{false};
    std::atomic<size_t> popped{0};
    std::atomic<bool> badChain{false};

    std::vector<std::thread> producers;

    // Element producers: range [0, kTotalElem).
    for (size_t p = 0; p < kElemProducers; p++) {
        producers.emplace_back([&, p] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < kPerElemProducer; i++) {
                size_t idx = p * kPerElemProducer + i;
                stack.push(pool[idx]);
            }
        });
    }

    // Chain producers: range [kTotalElem, kTotal). Build 8-chains locally,
    // then pushChain.
    for (size_t p = 0; p < kChainProducers; p++) {
        producers.emplace_back([&, p] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < kPerChainProducer; i++) {
                size_t base = kTotalElem
                            + (p * kPerChainProducer + i) * kChainDepth;
                // Link the chain locally before publishing.
                for (uint32_t j = 0; j + 1 < kChainDepth; j++) {
                    pool[base + j].chainNext = &pool[base + j + 1];
                }
                pool[base + kChainDepth - 1].chainNext = nullptr;
                pool[base].chainDepth = kChainDepth;
                stack.pushChain(pool[base], kChainDepth);
            }
        });
    }

    std::vector<std::vector<int>> consumerPayloads(kConsumers);
    std::vector<std::thread> consumers;
    for (size_t c = 0; c < kConsumers; c++) {
        consumers.emplace_back([&, c] {
            while (!startFlag.load(std::memory_order_acquire)) std::this_thread::yield();
            std::vector<int>& mine = consumerPayloads[c];
            while (true) {
                auto chain = stack.pop();
                if (chain.head) {
                    uint32_t walked = 0;
                    TestNode* n = chain.head;
                    while (n) {
                        mine.push_back(n->payload);
                        walked++;
                        TestNode* next = n->chainNext.load(RELAXED);
                        if (next == nullptr) break;
                        n = next;
                    }
                    if (walked != chain.depth) badChain.store(true);
                    // pushChain ignores maxChainLength so depths up to
                    // kChainDepth (8) are possible for pushChain'd chains —
                    // even if max is 8. The element-push path stays <= kMax.
                    popped.fetch_add(walked, std::memory_order_relaxed);
                } else if (popped.load() >= kTotal) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    startFlag.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    ASSERT_EQ(popped.load(), kTotal);
    ASSERT_FALSE(badChain.load());
    ASSERT_TRUE(stack.empty());

    // Multiset equality + uniqueness: every payload appears exactly once.
    std::vector<int> all;
    all.reserve(kTotal);
    for (auto& v : consumerPayloads) all.insert(all.end(), v.begin(), v.end());
    ASSERT_EQ(all.size(), kTotal);
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < kTotal; i++) ASSERT_EQ(all[i], static_cast<int>(i));
}

// ============================================================
// Concurrent stress — ABA scenario
// ============================================================
//
// Classic ABA pattern: thread A pops node X, frees it locally, pushes it back;
// concurrently thread B pops, expecting a coherent stack. With Uint128's 64-bit
// tag we cannot exhaust the tag in any reasonable test run, so this verifies
// the no-corruption property rather than tag-wrap behavior.
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(TreiberStack_Concurrent_AbaScenarioNoCorruption, 10000) {
    constexpr size_t kRoundTrips = 200000;

    TreiberStack<TestNode, TestNodeLinkage, DefaultEncoding> stack;
    TestNode a, b, c;
    a.payload = 1;
    b.payload = 2;
    c.payload = 3;
    stack.push(a);
    stack.push(b);
    stack.push(c);

    std::atomic<bool> stop{false};
    std::atomic<bool> badState{false};

    std::thread thrasher([&] {
        // Repeatedly pop the top and push it back; this is the classic ABA
        // producer of pop X / push X cycles.
        for (size_t i = 0; i < kRoundTrips; i++) {
            TestNode* n = stack.pop();
            if (!n) continue;
            stack.push(*n);
        }
        stop.store(true);
    });

    std::thread observer([&] {
        while (!stop.load()) {
            TestNode* n = stack.pop();
            if (n) {
                int v = n->payload;
                if (v != 1 && v != 2 && v != 3) badState.store(true);
                stack.push(*n);
            }
        }
    });

    thrasher.join();
    observer.join();

    ASSERT_FALSE(badState.load());

    // Final state: the three nodes are still on the stack (order unspecified).
    std::set<int> seen;
    while (true) {
        TestNode* n = stack.pop();
        if (!n) break;
        seen.insert(n->payload);
    }
    ASSERT_EQ(seen.size(), size_t(3));
    ASSERT_EQ(seen.count(1), size_t(1));
    ASSERT_EQ(seen.count(2), size_t(1));
    ASSERT_EQ(seen.count(3), size_t(1));
}
