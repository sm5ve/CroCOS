//
// Created by Spencer Martin on 3/1/26.
//

#ifndef CROCOS_ATOMIC_INTRUSIVE_LINKEDLIST_H
#define CROCOS_ATOMIC_INTRUSIVE_LINKEDLIST_H

#include <core/utility.h>
#include <core/atomic.h>
#include <stdint.h>

// An Extractor must provide:
//   - previous(node) -> Atomic<Node*>&  : ref to the node's atomic prev pointer
//   - next(node)     -> Atomic<Node*>&  : ref to the node's atomic next pointer
//   - nodeLock(node) -> LockType&       : ref to the node's embedded lock
//   - lockWriter(lock) / unlockWriter(lock)  : exclusive acquisition
//   - lockReader(lock) / unlockReader(lock)  : shared acquisition (reserved for
//                                              a future read-only traversal API)
//   - tryLockWriter(lock) -> bool            : non-blocking exclusive try-lock
template <typename Node, typename LockType, typename Extractor>
concept AtomicLinkedListNodeExtractor = requires(Node& node, LockType& lock){
    {Extractor::previous(node)} -> convertible_to<Atomic<Node*>&>;
    requires IsReference<decltype(Extractor::previous(node))>;
    {Extractor::next(node)} -> convertible_to<Atomic<Node*>&>;
    requires IsReference<decltype(Extractor::next(node))>;
    {Extractor::nodeLock(node)} -> convertible_to<LockType&>;
    { Extractor::lockWriter(lock) };
    { Extractor::unlockWriter(lock) };
    { Extractor::lockReader(lock) };
    { Extractor::unlockReader(lock) };
    {Extractor::tryLockWriter(lock)} -> convertible_to<bool>;
};

// ============================================================================
// Statistics levels for AtomicIntrusiveLinkedList.
//
// StatsNone   — zero overhead: no counters, no storage, no try-lock probes.
// StatsCoarse — aggregate counters across all operations.
// StatsFull   — per-operation breakdown for detailed tuning.
//
// The level is a compile-time template parameter; `if constexpr` eliminates
// all stat-update code when disabled, and `[[no_unique_address]]` on the stats
// member ensures zero storage for StatsNone.
// ============================================================================

enum StatisticsLevel { StatsNone, StatsCoarse, StatsFull };

struct NoStatistics {};

struct CoarseStatistics {
    alignas(64) Atomic<uint64_t> totalCalls{0};
    Atomic<uint64_t> totalContentions{0};
    Atomic<uint64_t> totalRetries{0};
    Atomic<uint64_t> totalEmpty{0};
    Atomic<uint64_t> currentSize{0};
    Atomic<uint64_t> peakSize{0};

    [[nodiscard]] uint64_t getTotalCalls()       const { return totalCalls.load(RELAXED); }
    [[nodiscard]] uint64_t getTotalContentions() const { return totalContentions.load(RELAXED); }
    [[nodiscard]] uint64_t getTotalRetries()     const { return totalRetries.load(RELAXED); }
    [[nodiscard]] uint64_t getTotalEmpty()       const { return totalEmpty.load(RELAXED); }
    [[nodiscard]] uint64_t getCurrentSize()      const { return currentSize.load(RELAXED); }
    [[nodiscard]] uint64_t getPeakSize()         const { return peakSize.load(RELAXED); }
};

struct FullStatistics {
    alignas(64) Atomic<uint64_t> popFrontCalls{0};
    Atomic<uint64_t> popFrontEmpty{0};
    Atomic<uint64_t> popFrontContentions{0};

    Atomic<uint64_t> popBackCalls{0};
    Atomic<uint64_t> popBackEmpty{0};
    Atomic<uint64_t> popBackContentions{0};
    Atomic<uint64_t> popBackRetries{0};

    Atomic<uint64_t> pushFrontCalls{0};
    Atomic<uint64_t> pushFrontContentions{0};

    Atomic<uint64_t> pushBackCalls{0};
    Atomic<uint64_t> pushBackContentions{0};
    Atomic<uint64_t> pushBackRetries{0};

    Atomic<uint64_t> removeCalls{0};
    Atomic<uint64_t> removeNotFound{0};
    Atomic<uint64_t> removeRetries{0};
    Atomic<uint64_t> removeContentions{0};

    Atomic<uint64_t> currentSize{0};
    Atomic<uint64_t> peakSize{0};

    // Aggregate accessors — compatible with CoarseStatistics interface.
    // Values are computed as sums of per-operation counters, so they may be
    // momentarily inconsistent under concurrent updates.  This is acceptable
    // for monotonic performance counters.
    [[nodiscard]] uint64_t getTotalCalls() const {
        return popFrontCalls.load(RELAXED) + popBackCalls.load(RELAXED)
             + pushFrontCalls.load(RELAXED) + pushBackCalls.load(RELAXED)
             + removeCalls.load(RELAXED);
    }
    [[nodiscard]] uint64_t getTotalContentions() const {
        return popFrontContentions.load(RELAXED) + popBackContentions.load(RELAXED)
             + pushFrontContentions.load(RELAXED) + pushBackContentions.load(RELAXED)
             + removeContentions.load(RELAXED);
    }
    [[nodiscard]] uint64_t getTotalRetries() const {
        return popBackRetries.load(RELAXED) + pushBackRetries.load(RELAXED)
             + removeRetries.load(RELAXED);
    }
    [[nodiscard]] uint64_t getTotalEmpty() const {
        return popFrontEmpty.load(RELAXED) + popBackEmpty.load(RELAXED)
             + removeNotFound.load(RELAXED);
    }
    [[nodiscard]] uint64_t getCurrentSize() const { return currentSize.load(RELAXED); }
    [[nodiscard]] uint64_t getPeakSize()    const { return peakSize.load(RELAXED); }
};

template <StatisticsLevel L>
using StatsType = conditional_t<L == StatsNone, NoStatistics,
                  conditional_t<L == StatsCoarse, CoarseStatistics,
                                FullStatistics>>;

// An intrusive doubly-linked list with per-node locking.
//
// Lock ordering is strictly left-to-right along the list:
//
//   headLock < node[0].lock < node[1].lock < ... < node[n].lock < tailLock < tailInsertLock
//
// headLock acts as the "previous" sentinel for the head node (i.e., it is
// acquired in place of a predecessor lock when operating on the front of the
// list). tailLock is the symmetric "next" sentinel for the tail.
//
// tailInsertLock is the rightmost lock in the total order.  It serializes
// pushBack and popBack, allowing them to stabilize the tail without a retry
// loop for mutual contention.  Only concurrent remove() of the tail node can
// still cause a retry inside the tailInsertLock critical section.
//
// `head` and `tail` are Atomic<Node*>: mutations still happen under the
// respective sentinel lock (ensuring only one writer at a time), but readers
// can load a candidate value lock-free.  Individual node locks guard that
// node's `prev` and `next` pointer fields.
//
// Node pointer semantics:
//   - nullptr in prev/next means the node is DETACHED (not in any list).
//   - SENTINEL in prev means the node is the head (no predecessor).
//   - SENTINEL in next means the node is the tail (no successor).
//   - Any other value is a pointer to the adjacent node.
//   This distinction allows remove() to detect double-remove (prev == nullptr)
//   and cross-list removal (SENTINEL but head/tail doesn't match) without an
//   owner pointer that would make splice O(n).
//
// Memory ordering discipline:
//   - Reads inside a locked section use RELAXED; the lock acquire/release
//     provides the necessary happens-before without an additional fence.
//   - Field writes to a node not yet in the list use RELAXED; they will be
//     flushed by the subsequent RELEASE store that publishes head/tail.
//   - Field writes inside a node's own lock use RELAXED; the lock release
//     (RELEASE fence) makes them visible before the lock is dropped.
//   - head/tail stores that publish new endpoints use RELEASE so that lockless
//     ACQUIRE peeks synchronize-with the store.
//   - The one exception: N.prev in popFront is written with RELEASE rather than
//     RELAXED because nlock(*N) is intentionally not held at that point (see
//     comment in popFront).
//
// Statistics:
//   The StatsLevel template parameter selects compile-time statistics tracking.
//   StatsNone (default) adds zero overhead — all stat code is eliminated by
//   if constexpr and the stats member occupies no storage via
//   [[no_unique_address]].  StatsCoarse tracks aggregate counters; StatsFull
//   provides per-operation breakdown.  Contention is detected by try-locking
//   before blocking on operations that always block-lock.
template <typename Node, typename LockType, typename Extractor,
          StatisticsLevel StatsLevel = StatsNone>
requires AtomicLinkedListNodeExtractor<Node, LockType, Extractor>
class AtomicIntrusiveLinkedList {
    // Head-side state on its own cache line to avoid false sharing with tail.
    alignas(64) Atomic<Node*> head{nullptr};
    LockType headLock;

    // Tail-side state on a separate cache line.
    alignas(64) Atomic<Node*> tail{nullptr};
    LockType tailLock;
    LockType tailInsertLock;

    // Statistics storage.  Zero bytes when StatsLevel == StatsNone.
    [[no_unique_address]] StatsType<StatsLevel> stats;

    // --- compile-time statistics flags ---------------------------------------

    static constexpr bool trackCoarse = StatsLevel >= StatsCoarse;
    static constexpr bool trackFull   = StatsLevel == StatsFull;

    // --- statistics helpers --------------------------------------------------

    static void statInc(Atomic<uint64_t>& c) { c.add_fetch(1, RELAXED); }
    static void statDec(Atomic<uint64_t>& c) { c.sub_fetch(1, RELAXED); }

    void updatePeak() {
        uint64_t cur = stats.currentSize.load(RELAXED);
        uint64_t peak = stats.peakSize.load(RELAXED);
        while (cur > peak) {
            if (stats.peakSize.compare_exchange(peak, cur, RELAXED, RELAXED))
                break;
        }
    }

    void statSizeInc() {
        statInc(stats.currentSize);
        updatePeak();
    }

    void statSizeDec() {
        statDec(stats.currentSize);
    }

    // Contention-detecting lock: try first, count contention, then block.
    // Used for locks that normally block unconditionally.
    void lockWContention(LockType& l, Atomic<uint64_t>& contentionCounter) {
        if (!tryW(l)) {
            statInc(contentionCounter);
            lockW(l);
        }
    }

    // --- constants -----------------------------------------------------------

    // Sentinel value stored in node prev/next to mark list endpoints.
    // Distinct from nullptr (which means "detached / not in any list").
    static Node* sentinel() { return reinterpret_cast<Node*>(~0ULL); }

    // True if `p` is a real node pointer (not nullptr and not sentinel).
    static bool isNode(Node* p) { return p != nullptr && p != sentinel(); }

    // Maximum pause iterations for exponential backoff in retry loops.
    static constexpr unsigned maxBackoff = 16;

    // --- lock helpers -------------------------------------------------------

    static void lockW(LockType& l)   { Extractor::lockWriter(l);   }
    static void unlockW(LockType& l) { Extractor::unlockWriter(l); }
    // lockR / unlockR are reserved for a future read-only traversal API.
    static void lockR(LockType& l)   { Extractor::lockReader(l);   }
    static void unlockR(LockType& l) { Extractor::unlockReader(l); }

    static bool tryW(LockType& l)    { return Extractor::tryLockWriter(l); }

    static LockType& nlock(Node& n)  { return Extractor::nodeLock(n); }

    // Lock all sentinel locks of two lists in address order to prevent deadlock.
    // Within a list the order is: headLock < tailLock < tailInsertLock.
    // Between lists the lower-addressed list's sentinels come first.
    // `mask` selects which locks to acquire:
    //   bit 0 = headLock, bit 1 = tailLock, bit 2 = tailInsertLock
    static void lockSentinelPair(AtomicIntrusiveLinkedList& a, unsigned maskA,
                                 AtomicIntrusiveLinkedList& b, unsigned maskB) {
        auto lockMask = [](AtomicIntrusiveLinkedList& l, unsigned m) {
            if (m & 1u) lockW(l.headLock);
            if (m & 2u) lockW(l.tailLock);
            if (m & 4u) lockW(l.tailInsertLock);
        };
        if (reinterpret_cast<uintptr_t>(&a) < reinterpret_cast<uintptr_t>(&b)) {
            lockMask(a, maskA);
            lockMask(b, maskB);
        } else {
            lockMask(b, maskB);
            lockMask(a, maskA);
        }
    }

    static void unlockSentinelPair(AtomicIntrusiveLinkedList& a, unsigned maskA,
                                   AtomicIntrusiveLinkedList& b, unsigned maskB) {
        auto unlockMask = [](AtomicIntrusiveLinkedList& l, unsigned m) {
            if (m & 4u) unlockW(l.tailInsertLock);
            if (m & 2u) unlockW(l.tailLock);
            if (m & 1u) unlockW(l.headLock);
        };
        // Unlock in reverse of lock order.
        if (reinterpret_cast<uintptr_t>(&a) < reinterpret_cast<uintptr_t>(&b)) {
            unlockMask(b, maskB);
            unlockMask(a, maskA);
        } else {
            unlockMask(a, maskA);
            unlockMask(b, maskB);
        }
    }

public:
    AtomicIntrusiveLinkedList() = default;

    // -------------------------------------------------------------------------
    // pushFront
    //
    // Inserts `node` before the current head.
    //
    // headLock is held for the entire operation, so `head` is stable once read
    // and no retry loop is needed.
    //
    // `node.lock` is NOT acquired: the incoming node is not yet visible to any
    // other thread, so no concurrent access to its fields is possible.  Its
    // fields are written (RELAXED) before head.store(RELEASE) publishes it;
    // the RELEASE fence guarantees they will be visible to any thread that
    // subsequently ACQUIRE-loads `head`.
    //
    // Lock order (non-empty): headLock -> oldHead.lock
    // Lock order (empty):     headLock -> tailLock
    // -------------------------------------------------------------------------
    void pushFront(Node& node) {
        if constexpr (trackFull) {
            statInc(stats.pushFrontCalls);
            lockWContention(headLock, stats.pushFrontContentions);
        } else if constexpr (trackCoarse) {
            statInc(stats.totalCalls);
            lockWContention(headLock, stats.totalContentions);
        } else {
            lockW(headLock);
        }

        // RELAXED: we hold headLock, which was last released after the prior write.
        Node* H = head.load(RELAXED);

        if (H == nullptr) {
            // Empty list: set both endpoints atomically.
            lockW(tailLock);
            // RELAXED: not yet visible; published by the RELEASE stores below.
            Extractor::previous(node).store(sentinel(), RELAXED);
            Extractor::next(node).store(sentinel(), RELAXED);
            head.store(&node, RELEASE);
            tail.store(&node, RELEASE);
            unlockW(tailLock);
            unlockW(headLock);
            if constexpr (trackCoarse) statSizeInc();
            return;
        }

        // Non-empty: splice node in before H.
        // node is not yet in the list; its lock is not needed.
        // Lock order: headLock (already held) -> H.lock
        lockW(nlock(*H));
        // RELAXED: not yet visible; published by the RELEASE store on head below.
        Extractor::previous(node).store(sentinel(), RELAXED);
        Extractor::next(node).store(H, RELAXED);
        // RELAXED: inside nlock(*H).  H.prev transitions from SENTINEL to &node.
        Extractor::previous(*H).store(&node, RELAXED);
        head.store(&node, RELEASE);
        unlockW(nlock(*H));
        unlockW(headLock);
        if constexpr (trackCoarse) statSizeInc();
    }

    // -------------------------------------------------------------------------
    // pushBack
    //
    // Appends `node` after the current tail.
    //
    // tailInsertLock is held for the entire operation, serializing all pushBack
    // (and popBack) callers.  This eliminates the retry loop for mutual
    // pushBack contention.
    //
    // A concurrent remove() of the tail node can still change `tail` while we
    // hold tailInsertLock (remove does not acquire tailInsertLock).  In that
    // rare case the validation check under tailLock catches the race and we
    // retry within the tailInsertLock critical section.
    //
    // `node.lock` is NOT acquired: the incoming node is not yet visible to any
    // other thread.  Its fields are written (RELAXED) before tail.store(RELEASE)
    // publishes it.
    //
    // Lock order (non-empty): tailInsertLock -> oldTail.lock -> tailLock
    // Lock order (empty):     tailInsertLock -> headLock -> tailLock
    // -------------------------------------------------------------------------
    void pushBack(Node& node) {
        if constexpr (trackFull) {
            statInc(stats.pushBackCalls);
            lockWContention(tailInsertLock, stats.pushBackContentions);
        } else if constexpr (trackCoarse) {
            statInc(stats.totalCalls);
            lockWContention(tailInsertLock, stats.totalContentions);
        } else {
            lockW(tailInsertLock);
        }

        while (true) {
            // ACQUIRE: synchronize with concurrent remove() that may have
            // changed tail without holding tailInsertLock.
            Node* T = tail.load(ACQUIRE);

            if (T == nullptr) {
                // Empty: acquire both sentinel locks (left-to-right).
                lockW(headLock);
                lockW(tailLock);
                // RELAXED: we hold all sentinel locks.
                if (tail.load(RELAXED) != nullptr) {
                    // A concurrent pushFront made the list non-empty — retry.
                    unlockW(tailLock);
                    unlockW(headLock);
                    if constexpr (trackFull)
                        statInc(stats.pushBackRetries);
                    else if constexpr (trackCoarse)
                        statInc(stats.totalRetries);
                    continue;
                }
                // RELAXED: not yet visible; published by the RELEASE stores below.
                Extractor::previous(node).store(sentinel(), RELAXED);
                Extractor::next(node).store(sentinel(), RELAXED);
                head.store(&node, RELEASE);
                tail.store(&node, RELEASE);
                unlockW(tailLock);
                unlockW(headLock);
                unlockW(tailInsertLock);
                if constexpr (trackCoarse) statSizeInc();
                return;
            }

            // Non-empty: acquire in left-to-right order, then validate.
            lockW(nlock(*T));
            lockW(tailLock);

            // RELAXED: we hold nlock(*T) and tailLock.
            if (tail.load(RELAXED) != T) {
                // Tail changed (concurrent remove of tail) — retry.
                unlockW(tailLock);
                unlockW(nlock(*T));
                if constexpr (trackFull)
                    statInc(stats.pushBackRetries);
                else if constexpr (trackCoarse)
                    statInc(stats.totalRetries);
                continue;
            }

            // RELAXED: not yet visible; published by the RELEASE store on tail below.
            Extractor::previous(node).store(T, RELAXED);
            Extractor::next(node).store(sentinel(), RELAXED);
            // RELAXED: inside nlock(*T).  T.next transitions from SENTINEL to &node.
            Extractor::next(*T).store(&node, RELAXED);
            tail.store(&node, RELEASE);

            unlockW(tailLock);
            unlockW(nlock(*T));
            unlockW(tailInsertLock);
            if constexpr (trackCoarse) statSizeInc();
            return;
        }
    }

    // -------------------------------------------------------------------------
    // remove
    //
    // Unlinks `node` from whatever position it occupies in this list.
    // Returns true if the node was removed, false if it was not in this list
    // (already removed, or belongs to a different list).
    //
    // Phase 1 reads node.prev with a lockless ACQUIRE load.  A stale value is
    // safe: the validation in Phase 3 catches it and retries.  A nullptr value
    // means the node is detached (not in any list) and we return immediately.
    //
    // We need the prev, node, and next locks simultaneously.  Prev must be
    // acquired before node (lock ordering), so we read node.prev, acquire the
    // prev sentinel, then acquire node.lock and re-validate prev.  Once
    // node.lock is held, next cannot change, so it is safe to lock next without
    // further validation.
    //
    // When prev is SENTINEL (node claims to be head), we verify head == &node.
    // When next is SENTINEL (node claims to be tail), we verify tail == &node.
    // If either check fails, the node belongs to a different list and we
    // return false.
    //
    // Lock order: prevNode.lock (or headLock) -> node.lock -> nextNode.lock (or tailLock)
    // -------------------------------------------------------------------------
    bool remove(Node& node) {
        if constexpr (trackFull)
            statInc(stats.removeCalls);
        else if constexpr (trackCoarse)
            statInc(stats.totalCalls);

        while (true) {
            // Phase 1: lockless read of prev. Stale value is safe — validated
            // in Phase 3. nullptr means the node is detached.
            Node* P = Extractor::previous(node).load(ACQUIRE);
            if (P == nullptr) {
                if constexpr (trackFull)
                    statInc(stats.removeNotFound);
                else if constexpr (trackCoarse)
                    statInc(stats.totalEmpty);
                return false;
            }

            // Phase 2: acquire the prev sentinel (left of node).
            if constexpr (trackFull) {
                if (isNode(P)) lockWContention(nlock(*P), stats.removeContentions);
                else           lockWContention(headLock, stats.removeContentions);
            } else if constexpr (trackCoarse) {
                if (isNode(P)) lockWContention(nlock(*P), stats.totalContentions);
                else           lockWContention(headLock, stats.totalContentions);
            } else {
                if (isNode(P)) lockW(nlock(*P));
                else           lockW(headLock);   // P == sentinel()
            }

            // Phase 3: re-acquire node and validate prev is still correct.
            lockW(nlock(node));
            // RELAXED: inside P.lock (or headLock) + node.lock.
            Node* currentP = Extractor::previous(node).load(RELAXED);

            if (currentP != P) {
                // A concurrent operation changed node.prev — retry.
                unlockW(nlock(node));
                if (isNode(P)) unlockW(nlock(*P));
                else           unlockW(headLock);
                if constexpr (trackFull)
                    statInc(stats.removeRetries);
                else if constexpr (trackCoarse)
                    statInc(stats.totalRetries);
                continue;
            }

            // If node claims to be head, verify this list agrees.
            if (P == sentinel() && head.load(RELAXED) != &node) {
                unlockW(nlock(node));
                unlockW(headLock);
                if constexpr (trackFull)
                    statInc(stats.removeNotFound);
                else if constexpr (trackCoarse)
                    statInc(stats.totalEmpty);
                return false;
            }

            // Phase 4: read next now that node.lock is held (next is stable).
            // RELAXED: inside node.lock.
            Node* N = Extractor::next(node).load(RELAXED);

            // N == nullptr means the node is half-detached (should not happen
            // under correct usage).  Treat as not-in-list.
            if (N == nullptr) {
                unlockW(nlock(node));
                if (isNode(P)) unlockW(nlock(*P));
                else           unlockW(headLock);
                if constexpr (trackFull)
                    statInc(stats.removeNotFound);
                else if constexpr (trackCoarse)
                    statInc(stats.totalEmpty);
                return false;
            }

            // Phase 5: acquire the next sentinel (right of node).
            if (isNode(N)) {
                lockW(nlock(*N));
            } else {
                // N == sentinel(): node claims to be tail.  Verify.
                lockW(tailLock);
                if (tail.load(RELAXED) != &node) {
                    unlockW(tailLock);
                    unlockW(nlock(node));
                    if (isNode(P)) unlockW(nlock(*P));
                    else           unlockW(headLock);
                    if constexpr (trackFull)
                        statInc(stats.removeNotFound);
                    else if constexpr (trackCoarse)
                        statInc(stats.totalEmpty);
                    return false;
                }
            }

            // Unlink.
            if (isNode(P)) Extractor::next(*P).store(isNode(N) ? N : sentinel(), RELAXED);
            else           head.store(isNode(N) ? N : nullptr, RELEASE);

            if (isNode(N)) Extractor::previous(*N).store(isNode(P) ? P : sentinel(), RELAXED);
            else           tail.store(isNode(P) ? P : nullptr, RELEASE);

            Extractor::previous(node).store(nullptr, RELAXED);
            Extractor::next(node).store(nullptr, RELAXED);

            // Unlock right-to-left.
            if (isNode(N)) unlockW(nlock(*N));
            else           unlockW(tailLock);
            unlockW(nlock(node));
            if (isNode(P)) unlockW(nlock(*P));
            else           unlockW(headLock);

            if constexpr (trackCoarse) statSizeDec();
            return true;
        }
    }

    // -------------------------------------------------------------------------
    // popFront
    //
    // Removes and returns the head node, or nullptr if the list is empty.
    //
    // A lockless ACQUIRE check of head is performed first as a fast path: if
    // the list appears empty, we return immediately without acquiring headLock.
    // A concurrent push between the check and return is benign — it is
    // equivalent to this popFront completing just before the push arrived.
    //
    // headLock is held throughout the slow path, so `head` is stable and no
    // retry loop is needed.
    //
    // nlock(*N) (the new head's lock) is NOT acquired when writing N.prev:
    // headLock + nlock(*H) together exclude all concurrent writers of N.prev,
    // since any operation that writes N.prev must first hold either headLock
    // (pushFront) or nlock(*H) (any operation treating H as N's predecessor).
    // N.prev is stored with RELEASE rather than RELAXED so that concurrent
    // ACQUIRE loads of N.prev (e.g., in remove Phase 1) observe SENTINEL
    // immediately, without relying solely on the happens-before chain through
    // headLock.
    //
    // Lock order: headLock -> head.lock -> tailLock (single-node case only)
    //             (N.lock skipped for the N.prev update — see above)
    // -------------------------------------------------------------------------
    Node* popFront() {
        if constexpr (trackFull)
            statInc(stats.popFrontCalls);
        else if constexpr (trackCoarse)
            statInc(stats.totalCalls);

        // Fast path: avoid headLock when the list is visibly empty.
        if (head.load(ACQUIRE) == nullptr) {
            if constexpr (trackFull)
                statInc(stats.popFrontEmpty);
            else if constexpr (trackCoarse)
                statInc(stats.totalEmpty);
            return nullptr;
        }

        if constexpr (trackFull) {
            lockWContention(headLock, stats.popFrontContentions);
        } else if constexpr (trackCoarse) {
            lockWContention(headLock, stats.totalContentions);
        } else {
            lockW(headLock);
        }

        // RELAXED: we hold headLock.
        Node* H = head.load(RELAXED);

        if (H == nullptr) {
            unlockW(headLock);
            if constexpr (trackFull)
                statInc(stats.popFrontEmpty);
            else if constexpr (trackCoarse)
                statInc(stats.totalEmpty);
            return nullptr;
        }

        lockW(nlock(*H));
        // RELAXED: inside headLock + nlock(*H).
        Node* N = Extractor::next(*H).load(RELAXED);

        if (isNode(N)) {
            // nlock(*N) is not acquired: headLock + nlock(*H) exclude all
            // concurrent writers of N.prev (see comment above).
            // RELEASE: ensures N.prev = SENTINEL is immediately visible to
            // concurrent ACQUIRE loads without holding nlock(*N).
            Extractor::previous(*N).store(sentinel(), RELEASE);
            head.store(N, RELEASE);
        } else {
            // H was the only node (N == SENTINEL) — clear both endpoints.
            lockW(tailLock);
            head.store(nullptr, RELEASE);
            tail.store(nullptr, RELEASE);
            unlockW(tailLock);
        }

        // Mark H as detached.
        // RELAXED: inside nlock(*H).
        Extractor::previous(*H).store(nullptr, RELAXED);
        Extractor::next(*H).store(nullptr, RELAXED);
        unlockW(nlock(*H));
        unlockW(headLock);
        if constexpr (trackCoarse) statSizeDec();
        return H;
    }

    // -------------------------------------------------------------------------
    // popBack
    //
    // Removes and returns the tail node, or nullptr if the list is empty.
    //
    // A lockless ACQUIRE check of tail is performed first as a fast path: if
    // the list appears empty, we return immediately without acquiring
    // tailInsertLock.
    //
    // tailInsertLock is held for the entire slow path, serializing against
    // concurrent pushBack and other popBack callers.  A concurrent remove()
    // of the tail or its predecessor can still race, so a retry loop is kept
    // for validation.  try_lock is used for node locks to avoid blocking on
    // potentially-stale nodes, with exponential backoff on try_lock failure.
    //
    // Lock order: tailInsertLock -> prevOfTail.lock (or headLock) -> tail.lock -> tailLock
    // -------------------------------------------------------------------------
    Node* popBack() {
        if constexpr (trackFull)
            statInc(stats.popBackCalls);
        else if constexpr (trackCoarse)
            statInc(stats.totalCalls);

        // Fast path: avoid tailInsertLock when the list is visibly empty.
        if (tail.load(ACQUIRE) == nullptr) {
            if constexpr (trackFull)
                statInc(stats.popBackEmpty);
            else if constexpr (trackCoarse)
                statInc(stats.totalEmpty);
            return nullptr;
        }

        if constexpr (trackFull) {
            lockWContention(tailInsertLock, stats.popBackContentions);
        } else if constexpr (trackCoarse) {
            lockWContention(tailInsertLock, stats.totalContentions);
        } else {
            lockW(tailInsertLock);
        }

        unsigned backoff = 1;

        while (true) {
            // ACQUIRE: synchronize with concurrent operations that may have
            // changed tail without holding tailInsertLock.
            Node* T = tail.load(ACQUIRE);

            if (T == nullptr) {
                unlockW(tailInsertLock);
                if constexpr (trackFull)
                    statInc(stats.popBackEmpty);
                else if constexpr (trackCoarse)
                    statInc(stats.totalEmpty);
                return nullptr;
            }

            // Read T's prev with a lockless ACQUIRE load.  A stale value is safe:
            // the validation below catches it and retries.
            Node* P = Extractor::previous(*T).load(ACQUIRE);

            // P should be SENTINEL (T is the only node) or a real node.
            // nullptr means T was concurrently removed — retry.
            if (P == nullptr) {
                for (unsigned i = 0; i < backoff; ++i) tight_spin();
                if (backoff < maxBackoff) backoff <<= 1;
                if constexpr (trackFull)
                    statInc(stats.popBackRetries);
                else if constexpr (trackCoarse)
                    statInc(stats.totalRetries);
                continue;
            }

            // Acquire left-to-right: prev sentinel, then T, then tailLock.
            // Use try_lock on node locks since T or P may have gone stale.
            if (isNode(P)) {
                if (!tryW(nlock(*P))) {
                    for (unsigned i = 0; i < backoff; ++i) tight_spin();
                    if (backoff < maxBackoff) backoff <<= 1;
                    if constexpr (trackFull) {
                        statInc(stats.popBackContentions);
                        statInc(stats.popBackRetries);
                    } else if constexpr (trackCoarse) {
                        statInc(stats.totalContentions);
                        statInc(stats.totalRetries);
                    }
                    continue;
                }
            } else {
                // P == SENTINEL: T is the only node.
                lockW(headLock);
            }

            if (!tryW(nlock(*T))) {
                if (isNode(P)) unlockW(nlock(*P));
                else           unlockW(headLock);
                for (unsigned i = 0; i < backoff; ++i) tight_spin();
                if (backoff < maxBackoff) backoff <<= 1;
                if constexpr (trackFull) {
                    statInc(stats.popBackContentions);
                    statInc(stats.popBackRetries);
                } else if constexpr (trackCoarse) {
                    statInc(stats.totalContentions);
                    statInc(stats.totalRetries);
                }
                continue;
            }

            lockW(tailLock);

            // Validate: T must still be the tail and P still its predecessor.
            // RELAXED: we hold all three locks.
            if (tail.load(RELAXED) != T || Extractor::previous(*T).load(RELAXED) != P) {
                unlockW(tailLock);
                unlockW(nlock(*T));
                if (isNode(P)) unlockW(nlock(*P));
                else           unlockW(headLock);
                if constexpr (trackFull)
                    statInc(stats.popBackRetries);
                else if constexpr (trackCoarse)
                    statInc(stats.totalRetries);
                continue;
            }

            // Unlink T.
            if (isNode(P)) {
                // P becomes the new tail; its next transitions to SENTINEL.
                // RELAXED: inside nlock(*P).
                Extractor::next(*P).store(sentinel(), RELAXED);
            } else {
                // T was the only node — clear head too.
                head.store(nullptr, RELEASE);
            }
            tail.store(isNode(P) ? P : nullptr, RELEASE);

            // Mark T as detached.
            // RELAXED: inside nlock(*T).
            Extractor::previous(*T).store(nullptr, RELAXED);
            Extractor::next(*T).store(nullptr, RELAXED);

            unlockW(tailLock);
            unlockW(nlock(*T));
            if (isNode(P)) unlockW(nlock(*P));
            else           unlockW(headLock);
            unlockW(tailInsertLock);
            if constexpr (trackCoarse) statSizeDec();
            return T;
        }
    }

    // -------------------------------------------------------------------------
    // spliceFront
    //
    // Prepends all nodes from `source` before this list's head.  `source` is
    // left empty.  Splicing a list into itself is a no-op.
    //
    // Both lists' headLock + tailLock are acquired in address order to prevent
    // deadlock when two lists splice into each other concurrently.
    //
    // The inner nodes of the source chain don't need individual locking — once
    // source's head/tail are cleared under its sentinel locks, no concurrent
    // operation can reach them through source.  Their prev/next pointers
    // (including SENTINEL values on the source endpoints) are updated under
    // the destination's sentinel locks + the junction node's lock.
    // -------------------------------------------------------------------------
    void spliceFront(AtomicIntrusiveLinkedList& source) {
        if (this == &source) return;

        // Acquire both lists' headLock + tailLock in address order.
        // mask: bit 0 = headLock, bit 1 = tailLock
        constexpr unsigned kHeadTail = 0x3u;
        lockSentinelPair(*this, kHeadTail, source, kHeadTail);

        // Drain source. RELAXED: we hold source's sentinel locks.
        Node* srcHead = source.head.load(RELAXED);
        Node* srcTail = source.tail.load(RELAXED);

        if (srcHead == nullptr) {
            // Source is empty — nothing to splice.
            unlockSentinelPair(*this, kHeadTail, source, kHeadTail);
            return;
        }

        // Clear source. RELEASE: makes the cleared state visible to lockless peeks.
        source.head.store(nullptr, RELEASE);
        source.tail.store(nullptr, RELEASE);

        // Transfer size from source to this list.
        if constexpr (trackCoarse) {
            uint64_t srcSize = source.stats.currentSize.load(RELAXED);
            source.stats.currentSize.store(0, RELAXED);
            stats.currentSize.add_fetch(srcSize, RELAXED);
            updatePeak();
        }

        // Now splice the source chain into this list's front.
        // RELAXED: we hold this->headLock.
        Node* thisHead = head.load(RELAXED);

        if (thisHead == nullptr) {
            // This list is empty: source chain becomes the entire list.
            // srcHead.prev and srcTail.next are already SENTINEL from source.
            // We already hold tailLock.
            head.store(srcHead, RELEASE);
            tail.store(srcTail, RELEASE);
        } else {
            // Non-empty: splice srcTail.next -> thisHead, thisHead.prev -> srcTail.
            // srcHead.prev stays SENTINEL (it remains the head of the combined list).
            lockW(nlock(*thisHead));
            Extractor::next(*srcTail).store(thisHead, RELAXED);
            Extractor::previous(*thisHead).store(srcTail, RELAXED);
            head.store(srcHead, RELEASE);
            unlockW(nlock(*thisHead));
        }

        unlockSentinelPair(*this, kHeadTail, source, kHeadTail);
    }

    // -------------------------------------------------------------------------
    // spliceBack
    //
    // Appends all nodes from `source` after this list's tail.  `source` is
    // left empty.  Splicing a list into itself is a no-op.
    //
    // Source's headLock + tailLock are acquired to drain it.  This list's
    // tailInsertLock is acquired to stabilize the tail.  Address-based
    // ordering prevents deadlock between concurrent cross-splices.
    // -------------------------------------------------------------------------
    void spliceBack(AtomicIntrusiveLinkedList& source) {
        if (this == &source) return;

        // Acquire source's headLock+tailLock and this->headLock+tailLock+tailInsertLock
        // in address order.
        // this: headLock + tailLock + tailInsertLock (mask 0x7)
        // source: headLock + tailLock (mask 0x3)
        constexpr unsigned kAll = 0x7u;
        constexpr unsigned kHeadTail = 0x3u;
        lockSentinelPair(*this, kAll, source, kHeadTail);

        // Drain source. RELAXED: we hold source's sentinel locks.
        Node* srcHead = source.head.load(RELAXED);
        Node* srcTail = source.tail.load(RELAXED);

        if (srcHead == nullptr) {
            // Source is empty — nothing to splice.
            unlockSentinelPair(*this, kAll, source, kHeadTail);
            return;
        }

        // Clear source. RELEASE: makes the cleared state visible to lockless peeks.
        source.head.store(nullptr, RELEASE);
        source.tail.store(nullptr, RELEASE);

        // Transfer size from source to this list.
        if constexpr (trackCoarse) {
            uint64_t srcSize = source.stats.currentSize.load(RELAXED);
            source.stats.currentSize.store(0, RELAXED);
            stats.currentSize.add_fetch(srcSize, RELAXED);
            updatePeak();
        }

        // Now splice the source chain onto this list's back.
        // RELAXED: we hold this->headLock + tailLock + tailInsertLock.
        Node* thisTail = tail.load(RELAXED);

        if (thisTail == nullptr) {
            // This list is empty: source chain becomes the entire list.
            // srcHead.prev and srcTail.next are already SENTINEL from source.
            head.store(srcHead, RELEASE);
            tail.store(srcTail, RELEASE);
        } else {
            // Non-empty: splice thisTail.next -> srcHead, srcHead.prev -> thisTail.
            // srcTail.next stays SENTINEL (it remains the tail of the combined list).
            lockW(nlock(*thisTail));
            Extractor::next(*thisTail).store(srcHead, RELAXED);
            Extractor::previous(*srcHead).store(thisTail, RELAXED);
            tail.store(srcTail, RELEASE);
            unlockW(nlock(*thisTail));
        }

        unlockSentinelPair(*this, kAll, source, kHeadTail);
    }

    // -------------------------------------------------------------------------
    // Read-only accessors
    //
    // These return a lockless snapshot that may be immediately stale.
    // A non-null return does not guarantee a subsequent pop will succeed;
    // an empty() == true result does not guarantee the list is still empty.
    // -------------------------------------------------------------------------

    Node* getHead() { return head.load(ACQUIRE); }
    Node* getTail() { return tail.load(ACQUIRE); }

    [[nodiscard]] bool empty() { return head.load(ACQUIRE) == nullptr; }

    // -------------------------------------------------------------------------
    // Statistics accessor
    //
    // Only exists when statistics are enabled.  Returns a const reference to
    // the stats struct (CoarseStatistics or FullStatistics).
    // -------------------------------------------------------------------------
    const auto& getStatistics() const requires (StatsLevel != StatsNone) { return stats; }
};

#endif //CROCOS_ATOMIC_INTRUSIVE_LINKEDLIST_H
