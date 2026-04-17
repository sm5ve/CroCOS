//
// Created by Spencer Martin on 4/8/26.
//
// Mock implementation of AtomicBitPool using an IntrusiveLinkedList + global Spinlock.
// Used for performance comparison against the lock-free AtomicBitPool.
//
#include <harness/TestHarness.h>
#include <core/atomic/AtomicBitPool.h>
#include <core/ds/LinkedList.h>
#include <core/math.h>
#include <core/mem.h>
#include <stdint.h>

struct LinkedListNode {
    bool present;
    LinkedListNode* prev;
    LinkedListNode* next;
};

struct BitPoolNodeExtractor {
    static LinkedListNode*& previous(LinkedListNode& node) { return node.prev; }
    static LinkedListNode*& next(LinkedListNode& node) { return node.next; }
};

using List = IntrusiveLinkedList<LinkedListNode, BitPoolNodeExtractor>;

// Buffer layout: [Spinlock][List][LinkedListNode[capacity]]
static constexpr size_t kLockOffset  = 0;
static constexpr size_t kListOffset  = (sizeof(Spinlock) + alignof(List) - 1) & ~(alignof(List) - 1);
static constexpr size_t kNodesOffset = (kListOffset + sizeof(List) + alignof(LinkedListNode) - 1) & ~(alignof(LinkedListNode) - 1);

static Spinlock& getSpinlockFromStorage(void* storage) {
    return *reinterpret_cast<Spinlock*>(static_cast<uint8_t*>(storage) + kLockOffset);
}

static List& getLinkedListFromStorage(void* storage) {
    return *reinterpret_cast<List*>(static_cast<uint8_t*>(storage) + kListOffset);
}

static LinkedListNode* getNodesFromStorage(void* storage) {
    return reinterpret_cast<LinkedListNode*>(static_cast<uint8_t*>(storage) + kNodesOffset);
}

size_t AtomicBitPool::requiredBufferSize(size_t capacity, size_t entryStride) {
    capacity = (1ull << log2ceil(capacity));
    return kNodesOffset + capacity * sizeof(LinkedListNode);
}

AtomicBitPool::AtomicBitPool(size_t capacity, void* buffer, size_t stride) {
    capacity = (1ull << log2ceil(capacity));
    this->storage = buffer;
    this->entryStride = stride;
    this->levelCount = capacity;  // repurposed: stores capacity in the mock
    this->l1BitmapLog2Width = 0;
    // Spinlock and List have a valid zero state (unlocked / empty).
    // LinkedListNodes zero-initialize to {present=false, prev=null, next=null}.
    memset(storage, 0, requiredBufferSize(capacity, stride));
}

AtomicBitPool::AddResult AtomicBitPool::add(size_t absoluteIndex) {
    auto& lock = getSpinlockFromStorage(storage);
    auto& list = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);
    LockGuard<Spinlock> guard(lock);

    if (nodes[absoluteIndex].present)
        return AddResult::AlreadyPresent;

    bool wasEmpty = list.empty();
    nodes[absoluteIndex].present = true;
    list.pushFront(nodes[absoluteIndex]);
    return wasEmpty ? AddResult::AddedToEmpty : AddResult::AddedToNonempty;
}

AtomicBitPool::RemoveResult AtomicBitPool::remove(size_t absoluteIndex) {
    auto& lock = getSpinlockFromStorage(storage);
    auto& list = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);
    LockGuard<Spinlock> guard(lock);

    if (!nodes[absoluteIndex].present)
        return RemoveResult::NotPresent;

    nodes[absoluteIndex].present = false;
    list.remove(nodes[absoluteIndex]);
    return list.empty() ? RemoveResult::RemovedAndMadeEmpty : RemoveResult::RemovedAndStayedNonempty;
}

AtomicBitPool::GetResult AtomicBitPool::getAny(size_t threadId, size_t& outIndex, size_t maxRetries) {
    auto& lock = getSpinlockFromStorage(storage);
    auto& list = getLinkedListFromStorage(storage);
    auto* nodes = getNodesFromStorage(storage);
    LockGuard<Spinlock> guard(lock);

    LinkedListNode* node = list.popFront();
    if (node == nullptr)
        return GetResult::Empty;

    node->present = false;
    outIndex = static_cast<size_t>(node - nodes);
    return GetResult::Success;
}

#ifdef CROCOS_TESTING
bool AtomicBitPool::checkInvariants() const {
    return true;
}
#endif
