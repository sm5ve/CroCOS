//
// Created by Spencer Martin on 4/8/26.
//
// Mock implementation of AtomicBitPool using AtomicIntrusiveLinkedList.
// Used for performance comparison against the lock-free AtomicBitPool.
//
// Presence is tracked via the node's own `prev` pointer (nullptr = not in list),
// eliminating the separate `present` flag that caused a race between the CAS and
// the list operation.  `add` atomically claims a slot by CAS-ing `prev` from
// nullptr to SENTINEL before calling pushFront; this is safe because the list's
// `remove` already handles the "prev==SENTINEL but not yet head" case (returns
// false if head != &node).
//
#include <harness/TestHarness.h>
#include <core/atomic/AtomicBitPool.h>
#include <core/atomic/AtomicLinkedList.h>
#include <core/math.h>
#include <core/mem.h>
#include <stdint.h>

struct ANode {
    Atomic<ANode*> prev{nullptr};
    Atomic<ANode*> next{nullptr};
    RWSpinlock     lock;
};

struct ANodeExtractor {
    static Atomic<ANode*>& previous(ANode& n)    { return n.prev; }
    static Atomic<ANode*>& next(ANode& n)        { return n.next; }
    static RWSpinlock&     nodeLock(ANode& n)    { return n.lock; }
    static void   lockWriter(RWSpinlock& l)      { l.acquire_writer(); }
    static void   unlockWriter(RWSpinlock& l)    { l.release_writer(); }
    static void   lockReader(RWSpinlock& l)      { l.acquire_reader(); }
    static void   unlockReader(RWSpinlock& l)    { l.release_reader(); }
    static bool   tryLockWriter(RWSpinlock& l)   { return l.try_acquire_writer(); }
};

using List = AtomicIntrusiveLinkedList<ANode, RWSpinlock, ANodeExtractor>;

// The sentinel value the list uses to mark list endpoints (prev of head, next of tail).
// Matches AtomicIntrusiveLinkedList::sentinel() = reinterpret_cast<Node*>(~0ULL).
static ANode* const kSentinel = reinterpret_cast<ANode*>(~uintptr_t(0));

// Buffer layout: [List][ANode[capacity]]
static constexpr size_t kListOffset  = 0;
static constexpr size_t kNodesOffset = (sizeof(List) + alignof(ANode) - 1) & ~(alignof(ANode) - 1);

static List& getLinkedListFromStorage(void* storage) {
    return *reinterpret_cast<List*>(static_cast<uint8_t*>(storage) + kListOffset);
}

static ANode* getNodesFromStorage(void* storage) {
    return reinterpret_cast<ANode*>(static_cast<uint8_t*>(storage) + kNodesOffset);
}

size_t AtomicBitPool::requiredBufferSize(size_t capacity, size_t /*entryStride*/) {
    capacity = (1ull << log2ceil(capacity));
    return kNodesOffset + capacity * sizeof(ANode);
}

AtomicBitPool::AtomicBitPool(size_t capacity, void* buffer, size_t stride) {
    capacity = (1ull << log2ceil(capacity));
    this->storage = buffer;
    this->entryStride = stride;
    this->levelCount = capacity;  // repurposed: stores capacity in the mock
    this->l1BitmapLog2Width = 0;
    // AtomicIntrusiveLinkedList, RWSpinlock, and Atomic<T> all have a valid zero
    // state (empty list / unlocked / nullptr).
    memset(storage, 0, requiredBufferSize(capacity, stride));
}

AtomicBitPool::AddResult AtomicBitPool::add(size_t absoluteIndex) {
    auto& list  = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);

    // Claim the slot: CAS prev nullptr → kSentinel.
    // If the CAS fails, the node is already in the list (or pending insertion).
    ANode* expected = nullptr;
    if (!nodes[absoluteIndex].prev.compare_exchange(expected, kSentinel))
        return AddResult::AlreadyPresent;

    bool wasEmpty = list.empty();
    list.pushFront(nodes[absoluteIndex]);
    return wasEmpty ? AddResult::AddedToEmpty : AddResult::AddedToNonempty;
}

AtomicBitPool::RemoveResult AtomicBitPool::remove(size_t absoluteIndex) {
    auto& list  = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);

    // list.remove returns false if the node is detached (prev == nullptr)
    // or if it claims to be a head/tail but isn't (stale sentinel from a
    // pending add that hasn't called pushFront yet).
    if (!list.remove(nodes[absoluteIndex]))
        return RemoveResult::NotPresent;

    return list.empty() ? RemoveResult::RemovedAndMadeEmpty : RemoveResult::RemovedAndStayedNonempty;
}

AtomicBitPool::GetResult AtomicBitPool::getAny(size_t /*threadId*/, size_t& outIndex, size_t /*maxRetries*/) {
    auto& list  = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);

    ANode* node = list.popFront();
    if (node == nullptr)
        return GetResult::Empty;

    outIndex = static_cast<size_t>(node - nodes);
    return GetResult::Success;
}

#ifdef CROCOS_TESTING
bool AtomicBitPool::checkInvariants() const {
    return true;
}
#endif
