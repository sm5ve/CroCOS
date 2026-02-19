//
// Created by Spencer Martin on 4/30/25.
//

#ifndef CROCOS_RINGBUFFER_H
#define CROCOS_RINGBUFFER_H

#include <stddef.h>
#include <core/atomic.h>
#include <core/TypeTraits.h>
#include <core/utility.h>

namespace RingBufferInternal {

// Advance a completion head (writtenHead or readHead) after finishing a batch.
//
// Spin-waits until the head reaches `expected` (meaning all earlier batches
// have completed), then CAS-advances it to `desired` in a single operation.
// The RELEASE ordering on the CAS ensures all slot writes from this batch
// are visible to any thread that subsequently loads the head with ACQUIRE.
//
// Ordered completion is achieved by each batch waiting its turn to advance
// the head. No per-slot generation counters are needed.
inline void advanceCompletionHead(Atomic<size_t>& head, size_t expected, size_t desired) {
    size_t current = expected;
    while (!head.compare_exchange(current, desired, RELEASE, RELAXED)) {
        if (current >= desired) return; // Another thread already advanced past our range
        current = expected;
        tight_spin();
    }
}

// Advance a completion head using per-slot generation counters with early return.
//
// Producers that finish out of order can return immediately after storing their
// gen counters, without spin-waiting. Whoever successfully CAS-advances the head
// scans gen counters to pick up all subsequently completed batches in one shot.
//
// All gen counter stores, gen counter loads, and CAS operations on the head use
// SEQ_CST ordering. This is critical for portability: the SEQ_CST total order S
// constrains the visibility of gen counter stores to scanners on other threads.
//
// Proof sketch for correctness:
//   Producer B stores genCounters[slot] (SEQ_CST), then fails CAS on head
//   (SEQ_CST, reads value V). Producer A succeeds CAS on head (SEQ_CST, reads
//   V, writes V'). A then scans genCounters[slot] (SEQ_CST). All four operations
//   are in the total order S. Within each thread, sequenced-before forces ordering
//   in S. Between threads: if A's CAS preceded B's CAS in S, then B's CAS (a
//   SEQ_CST load) would have to read A's write V', not V — contradiction. So B's
//   CAS precedes A's CAS in S. Therefore: B's GC store < B's CAS < A's CAS <
//   A's GC load in S. A's load reads B's store. QED.
//
// On x86, SEQ_CST has zero additional cost (CAS is already LOCK CMPXCHG, a full
// barrier). On ARM64/RISC-V, SEQ_CST incurs stronger fence instructions — if
// benchmarking shows this is too costly, switch ScanOnComplete to false for the
// spin-wait approach instead.
inline void advanceCompletionHeadWithScan(Atomic<size_t>& head, const Atomic<size_t>* genCounters,
                                          size_t cap, size_t expected, size_t desired) {
    size_t current = expected;
    while (!head.compare_exchange(current, desired, SEQ_CST, SEQ_CST)) {
        if (current >= desired) return; // Another thread already advanced past our range
        if (current < expected) return; // Early return: no scanner has entered our range
        // expected <= current < desired: a scanner partially consumed our batch
        // (it saw some of our gen counter stores but not all). Retry CAS from the
        // updated current — our remaining gen counters are stored and we need to
        // advance the head past the rest of our range.
    }

    // CAS succeeded — we advanced head to desired (possibly from a value
    // between expected and desired if a scanner partially consumed our batch).
    // Scan ahead for subsequently completed batches.
    // After main CAS succeeds:
	size_t scanPos = desired;
	while (true) {
	    // Scan ahead
    	while (true) {
        	size_t slotIdx = scanPos % cap;
        	size_t expectedGen = scanPos / cap + 1;
        	if (genCounters[slotIdx].load(SEQ_CST) < expectedGen) break;
        	scanPos++;
    	}

    	if (scanPos == desired) break;  // Nothing found

    	// Try to advance head; if we fail, someone else took over
    	current = desired;
    	if (!head.compare_exchange(current, scanPos, SEQ_CST, SEQ_CST)) break;
    	desired = scanPos;  // Loop back and scan from new position
	}
}

} // namespace RingBufferInternal

// Three-head MPMC ring buffer for use cases with structural guarantees against overflow.
//
// Heads:
//   writeHead    - next slot to claim for writing
//   writtenHead  - next slot available for reading (published writes)
//   readHead     - next slot to claim for reading
//
// No protection against readers seeing stale data if the buffer overflows;
// the caller must structurally guarantee that overflow cannot occur.
//
// Template parameters:
//   T              - element type
//   Owning         - if true, buffer owns its backing storage
//   ScanOnComplete - if true, uses per-slot generation counters with SEQ_CST
//                    ordering so that out-of-order producers can return early
//                    and a single scanner coalesces all completed batches into
//                    one head advancement. If false, each producer spin-waits
//                    its turn (lower per-operation overhead, no gen counter arrays).
//
// Design note: all head pointers are monotonically increasing logical positions,
// mapped to physical slots via (head % capacity). This avoids ABA problems in
// CAS loops and simplifies generation tracking. This assumes fewer than 2^64
// total operations over the lifetime of the buffer, which is not reachable in
// practice (at 1 billion ops/sec, overflow would take ~585 years).
template<typename T, bool Owning = true, bool ScanOnComplete = false>
class SimpleMPMCRingBuffer {
    T* buffer;
    Atomic<size_t>* writeGenCounters;
    size_t cap;

    Atomic<size_t> writeHead{size_t(0)};
    Atomic<size_t> writtenHead{size_t(0)};
    Atomic<size_t> readHead{size_t(0)};

    bool tryClaimWrite(size_t count, size_t& claimed) {
        do {
            claimed = writeHead.load(ACQUIRE);
            size_t rh = readHead.load(ACQUIRE);
            if (count > cap - (claimed - rh)) return false;
        } while (!writeHead.compare_exchange(claimed, claimed + count, ACQUIRE, RELAXED));
        return true;
    }

    template<typename WriteCallback>
    void executeWrite(size_t claimed, size_t count, WriteCallback& callback) {
        for (size_t i = 0; i < count; i++) {
            callback(i, buffer[(claimed + i) % cap]);
        }
        if constexpr (ScanOnComplete) {
            for (size_t i = 0; i < count; i++) {
                size_t slotIdx = (claimed + i) % cap;
                size_t gen = (claimed + i) / cap + 1;
				writeGenCounters[slotIdx].store(gen, SEQ_CST);
            }
            RingBufferInternal::advanceCompletionHeadWithScan(
                writtenHead, writeGenCounters, cap, claimed, claimed + count);
        } else {
            RingBufferInternal::advanceCompletionHead(writtenHead, claimed, claimed + count);
        }
    }

    bool tryClaimRead(size_t count, size_t& claimed) {
        do {
            claimed = readHead.load(ACQUIRE);
            size_t wth = writtenHead.load(ACQUIRE);
            if (count > wth - claimed) return false;
        } while (!readHead.compare_exchange(claimed, claimed + count, ACQUIRE, RELAXED));
        return true;
    }

    template<typename ReadCallback>
    void executeRead(size_t claimed, size_t count, ReadCallback& callback) {
        for (size_t i = 0; i < count; i++) {
            callback(i, static_cast<const T&>(buffer[(claimed + i) % cap]));
        }
    }

    size_t claimBestEffortWrite(size_t maxCount, size_t& claimed) {
        size_t actualCount;
        do {
            claimed = writeHead.load(ACQUIRE);
            size_t rh = readHead.load(ACQUIRE);
            size_t available = cap - (claimed - rh);
            actualCount = min(maxCount, available);
            if (actualCount == 0) return 0;
        } while (!writeHead.compare_exchange(claimed, claimed + actualCount, ACQUIRE, RELAXED));
        return actualCount;
    }

    size_t claimBestEffortRead(size_t maxCount, size_t& claimed) {
        size_t actualCount;
        do {
            claimed = readHead.load(ACQUIRE);
            size_t wth = writtenHead.load(ACQUIRE);
            size_t available = wth - claimed;
            actualCount = min(maxCount, available);
            if (actualCount == 0) return 0;
        } while (!readHead.compare_exchange(claimed, claimed + actualCount, ACQUIRE, RELAXED));
        return actualCount;
    }

    // Non-copyable, non-movable
    SimpleMPMCRingBuffer(const SimpleMPMCRingBuffer&) = delete;
    SimpleMPMCRingBuffer& operator=(const SimpleMPMCRingBuffer&) = delete;

    template<typename U, bool O> friend class BroadcastRingBuffer;

public:
    // Owning constructor: allocates internal storage
    explicit SimpleMPMCRingBuffer(size_t capacity) requires (Owning)
        : writeGenCounters(nullptr), cap(capacity) {
        buffer = static_cast<T*>(operator new(sizeof(T) * capacity, std::align_val_t{alignof(T)}));
        if constexpr (ScanOnComplete) {
            writeGenCounters = new Atomic<size_t>[capacity]();
        }
    }

    // Non-owning constructor: borrows external buffer (no gen counters)
    SimpleMPMCRingBuffer(T* buf, size_t capacity) requires (!Owning && !ScanOnComplete)
        : buffer(buf), writeGenCounters(nullptr), cap(capacity) {}

    // Non-owning constructor: borrows external buffer and gen counter array
    SimpleMPMCRingBuffer(T* buf, size_t capacity, Atomic<size_t>* wgc) requires (!Owning && ScanOnComplete)
        : buffer(buf), writeGenCounters(wgc), cap(capacity) {}

    ~SimpleMPMCRingBuffer() {
        if constexpr (Owning) {
            size_t rh = readHead.load(RELAXED);
            size_t wth = writtenHead.load(RELAXED);
            if constexpr (!is_trivially_copyable_v<T>) {
                for (size_t i = rh; i < wth; i++) {
                    buffer[i % cap].~T();
                }
            }
            operator delete(buffer, std::align_val_t{alignof(T)});
            delete[] writeGenCounters;
        }
    }

    // All-or-nothing write. For SimpleMPMCRingBuffer this is identical to tryBulkWrite
    // since structural guarantees prevent overflow.
    template<typename WriteCallback>
    bool bulkWrite(size_t count, WriteCallback callback) {
        return tryBulkWrite(count, forward<WriteCallback>(callback));
    }

    // Non-blocking all-or-nothing write.
    // Returns true if successful, false if insufficient space.
    template<typename WriteCallback>
    bool tryBulkWrite(size_t count, WriteCallback callback) {
        size_t claimed;
        if (!tryClaimWrite(count, claimed)) return false;
        executeWrite(claimed, count, callback);
        return true;
    }

    // Best-effort: write up to count items, returns actual count written.
    template<typename WriteCallback>
    size_t bulkWriteBestEffort(size_t count, WriteCallback callback) {
        size_t claimed;
        size_t actualCount = claimBestEffortWrite(count, claimed);
        if (actualCount == 0) return 0;
        executeWrite(claimed, actualCount, callback);
        return actualCount;
    }

    // All-or-nothing read. For SimpleMPMCRingBuffer this is identical to tryBulkRead.
    template<typename ReadCallback>
    bool bulkRead(size_t count, ReadCallback callback) {
        return tryBulkRead(count, forward<ReadCallback>(callback));
    }

    // Non-blocking all-or-nothing read.
    // Returns true if successful, false if insufficient items available.
    template<typename ReadCallback>
    bool tryBulkRead(size_t count, ReadCallback callback) {
        size_t claimed;
        if (!tryClaimRead(count, claimed)) return false;
        executeRead(claimed, count, callback);
        return true;
    }

    // Best-effort: read up to count items, returns actual count read.
    template<typename ReadCallback>
    size_t bulkReadBestEffort(size_t count, ReadCallback callback) {
        size_t claimed;
        size_t actualCount = claimBestEffortRead(count, claimed);
        if (actualCount == 0) return 0;
        executeRead(claimed, actualCount, callback);
        return actualCount;
    }

    // Conservative estimate of slots available for writing.
    size_t availableToWrite() const {
        return cap - (writeHead.load(ACQUIRE) - readHead.load(ACQUIRE));
    }

    // Conservative estimate of slots available for reading.
    size_t availableToRead() const {
        return writtenHead.load(ACQUIRE) - readHead.load(ACQUIRE);
    }

    bool empty() const { return availableToRead() == 0; }
    bool full() const { return availableToWrite() == 0; }

    // Discard all readable items. Must not be called concurrently with
    // any other operation on this buffer.
    void clear() {
        if constexpr (Owning) {
            size_t rh = readHead.load(RELAXED);
            size_t wth = writtenHead.load(RELAXED);
            if constexpr (!is_trivially_copyable_v<T>) {
                for (size_t i = rh; i < wth; i++) {
                    buffer[i % cap].~T();
                }
            }
        }
        readHead.store(writtenHead.load(RELAXED), RELAXED);
    }
};


// Four-head MPMC ring buffer for general use where overflow must be prevented.
//
// Heads:
//   writeHead    - next slot to claim for writing
//   writtenHead  - next slot available for reading (published writes)
//   readingHead  - next slot to claim for reading
//   readHead     - fully consumed slot (read completion pointer)
//
// Key difference from SimpleMPMCRingBuffer:
//   bulkWrite checks against readingHead to assess space availability
//   (optimistic: reads are in-flight and will complete), then spin-waits
//   per-slot for readHead to catch up before actually writing.
//   tryBulkWrite checks against readHead (conservative: no spin-wait).
//
// Template parameters:
//   T              - element type
//   Owning         - if true, buffer owns its backing storage
//   ScanOnComplete - if true, uses per-slot generation counters with SEQ_CST
//                    ordering on both write and read completion heads for
//                    scan-ahead with early return. If false, uses simple spin-wait.
//
// See SimpleMPMCRingBuffer for the monotonic head pointer design note.
template<typename T, bool Owning = true, bool ScanOnComplete = false>
class MPMCRingBuffer {
    T* buffer;
    Atomic<size_t>* writeGenCounters;
    Atomic<size_t>* readGenCounters;
    size_t cap;

    Atomic<size_t> writeHead{size_t(0)};
    Atomic<size_t> writtenHead{size_t(0)};
    Atomic<size_t> readingHead{size_t(0)};
    Atomic<size_t> readHead{size_t(0)};

    // Wait for readHead to advance past a specific logical position,
    // ensuring the slot is safe to overwrite.
    void waitForSlotReadable(size_t logicalPos) {
        if (logicalPos < cap) return; // First lap, no previous occupant
        size_t requiredReadHead = logicalPos - cap + 1;
        while (readHead.load(ACQUIRE) < requiredReadHead) {
            tight_spin();
        }
    }

    // Optimistic claim: check against readingHead (reads are in-flight).
    // Used by bulkWrite which will spin-wait per-slot for readHead.
    bool claimWriteOptimistic(size_t count, size_t& claimed) {
        do {
            claimed = writeHead.load(ACQUIRE);
            size_t rgh = readingHead.load(ACQUIRE);
            if (count > cap - (claimed - rgh)) return false;
        } while (!writeHead.compare_exchange(claimed, claimed + count, ACQUIRE, RELAXED));
        return true;
    }

    // Conservative claim: check against readHead (reads fully complete).
    // Used by tryBulkWrite which won't spin-wait.
    bool claimWriteConservative(size_t count, size_t& claimed) {
        do {
            claimed = writeHead.load(ACQUIRE);
            size_t rh = readHead.load(ACQUIRE);
            if (count > cap - (claimed - rh)) return false;
        } while (!writeHead.compare_exchange(claimed, claimed + count, ACQUIRE, RELAXED));
        return true;
    }

    template<typename WriteCallback>
    void executeWriteWithWait(size_t claimed, size_t count, WriteCallback& callback) {
        for (size_t i = 0; i < count; i++) {
            size_t logicalPos = claimed + i;
            waitForSlotReadable(logicalPos);
            callback(i, buffer[logicalPos % cap]);
        }
        if constexpr (ScanOnComplete) {
            for (size_t i = 0; i < count; i++) {
                size_t slotIdx = (claimed + i) % cap;
                size_t gen = (claimed + i) / cap + 1;
                writeGenCounters[slotIdx].store(gen, SEQ_CST);
            }
            RingBufferInternal::advanceCompletionHeadWithScan(
                writtenHead, writeGenCounters, cap, claimed, claimed + count);
        } else {
            RingBufferInternal::advanceCompletionHead(writtenHead, claimed, claimed + count);
        }
    }

    template<typename WriteCallback>
    void executeWriteImmediate(size_t claimed, size_t count, WriteCallback& callback) {
        for (size_t i = 0; i < count; i++) {
            callback(i, buffer[(claimed + i) % cap]);
        }
        if constexpr (ScanOnComplete) {
            for (size_t i = 0; i < count; i++) {
                size_t slotIdx = (claimed + i) % cap;
                size_t gen = (claimed + i) / cap + 1;
                writeGenCounters[slotIdx].store(gen, SEQ_CST);
            }
            RingBufferInternal::advanceCompletionHeadWithScan(
                writtenHead, writeGenCounters, cap, claimed, claimed + count);
        } else {
            RingBufferInternal::advanceCompletionHead(writtenHead, claimed, claimed + count);
        }
    }

    bool tryClaimRead(size_t count, size_t& claimed) {
        do {
            claimed = readingHead.load(ACQUIRE);
            size_t wth = writtenHead.load(ACQUIRE);
            if (count > wth - claimed) return false;
        } while (!readingHead.compare_exchange(claimed, claimed + count, ACQUIRE, RELAXED));
        return true;
    }

    template<typename ReadCallback>
    void executeRead(size_t claimed, size_t count, ReadCallback& callback) {
        for (size_t i = 0; i < count; i++) {
            callback(i, static_cast<const T&>(buffer[(claimed + i) % cap]));
        }
        if constexpr (ScanOnComplete) {
            for (size_t i = 0; i < count; i++) {
                size_t slotIdx = (claimed + i) % cap;
                size_t gen = (claimed + i) / cap + 1;
                readGenCounters[slotIdx].store(gen, SEQ_CST);
            }
            RingBufferInternal::advanceCompletionHeadWithScan(
                readHead, readGenCounters, cap, claimed, claimed + count);
        } else {
            RingBufferInternal::advanceCompletionHead(readHead, claimed, claimed + count);
        }
    }

    size_t claimBestEffortWriteConservative(size_t maxCount, size_t& claimed) {
        size_t actualCount;
        do {
            claimed = writeHead.load(ACQUIRE);
            size_t rh = readHead.load(ACQUIRE);
            size_t available = cap - (claimed - rh);
            actualCount = min(maxCount, available);
            if (actualCount == 0) return 0;
        } while (!writeHead.compare_exchange(claimed, claimed + actualCount, ACQUIRE, RELAXED));
        return actualCount;
    }

    size_t claimBestEffortRead(size_t maxCount, size_t& claimed) {
        size_t actualCount;
        do {
            claimed = readingHead.load(ACQUIRE);
            size_t wth = writtenHead.load(ACQUIRE);
            size_t available = wth - claimed;
            actualCount = min(maxCount, available);
            if (actualCount == 0) return 0;
        } while (!readingHead.compare_exchange(claimed, claimed + actualCount, ACQUIRE, RELAXED));
        return actualCount;
    }

    // Non-copyable, non-movable
    MPMCRingBuffer(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;

public:
    // Owning constructor: allocates internal storage
    explicit MPMCRingBuffer(size_t capacity) requires (Owning)
        : writeGenCounters(nullptr), readGenCounters(nullptr), cap(capacity) {
        buffer = static_cast<T*>(operator new(sizeof(T) * capacity, std::align_val_t{alignof(T)}));
        if constexpr (ScanOnComplete) {
            writeGenCounters = new Atomic<size_t>[capacity]();
            readGenCounters = new Atomic<size_t>[capacity]();
        }
    }

    // Non-owning constructor: borrows external buffer (no gen counters)
    MPMCRingBuffer(T* buf, size_t capacity) requires (!Owning && !ScanOnComplete)
        : buffer(buf), writeGenCounters(nullptr), readGenCounters(nullptr), cap(capacity) {}

    // Non-owning constructor: borrows external buffer and both gen counter arrays
    MPMCRingBuffer(T* buf, size_t capacity, Atomic<size_t>* wgc, Atomic<size_t>* rgc) requires (!Owning && ScanOnComplete)
        : buffer(buf), writeGenCounters(wgc), readGenCounters(rgc), cap(capacity) {}

    ~MPMCRingBuffer() {
        if constexpr (Owning) {
            size_t rgh = readingHead.load(RELAXED);
            size_t wth = writtenHead.load(RELAXED);
            if constexpr (!is_trivially_copyable_v<T>) {
                for (size_t i = rgh; i < wth; i++) {
                    buffer[i % cap].~T();
                }
            }
            operator delete(buffer, std::align_val_t{alignof(T)});
            delete[] writeGenCounters;
            delete[] readGenCounters;
        }
    }

    // All-or-nothing write with spin-wait.
    // Checks against readingHead for space (optimistic), then spin-waits
    // per-slot for readHead to ensure the slot is safe to overwrite.
    // Returns true if successful, false if insufficient space.
    template<typename WriteCallback>
    bool bulkWrite(size_t count, WriteCallback callback) {
        size_t claimed;
        if (!claimWriteOptimistic(count, claimed)) return false;
        executeWriteWithWait(claimed, count, callback);
        return true;
    }

    // Non-blocking all-or-nothing write.
    // Checks against readHead (conservative). No spin-wait needed.
    // Returns true if successful, false if insufficient space.
    template<typename WriteCallback>
    bool tryBulkWrite(size_t count, WriteCallback callback) {
        size_t claimed;
        if (!claimWriteConservative(count, claimed)) return false;
        executeWriteImmediate(claimed, count, callback);
        return true;
    }

    // Best-effort: write up to count items, returns actual count written.
    // Uses conservative check (readHead) to avoid spin-waiting.
    template<typename WriteCallback>
    size_t bulkWriteBestEffort(size_t count, WriteCallback callback) {
        size_t claimed;
        size_t actualCount = claimBestEffortWriteConservative(count, claimed);
        if (actualCount == 0) return 0;
        executeWriteImmediate(claimed, actualCount, callback);
        return actualCount;
    }

    // All-or-nothing read.
    // Returns true if successful, false if insufficient items available.
    template<typename ReadCallback>
    bool bulkRead(size_t count, ReadCallback callback) {
        return tryBulkRead(count, forward<ReadCallback>(callback));
    }

    // Non-blocking all-or-nothing read.
    // Returns true if successful, false if insufficient items available.
    template<typename ReadCallback>
    bool tryBulkRead(size_t count, ReadCallback callback) {
        size_t claimed;
        if (!tryClaimRead(count, claimed)) return false;
        executeRead(claimed, count, callback);
        return true;
    }

    // Best-effort: read up to count items, returns actual count read.
    template<typename ReadCallback>
    size_t bulkReadBestEffort(size_t count, ReadCallback callback) {
        size_t claimed;
        size_t actualCount = claimBestEffortRead(count, claimed);
        if (actualCount == 0) return 0;
        executeRead(claimed, actualCount, callback);
        return actualCount;
    }

    // Conservative estimate of slots available for writing.
    size_t availableToWrite() const {
        return cap - (writeHead.load(ACQUIRE) - readHead.load(ACQUIRE));
    }

    // Conservative estimate of slots available for reading.
    size_t availableToRead() const {
        return writtenHead.load(ACQUIRE) - readingHead.load(ACQUIRE);
    }

    bool empty() const { return availableToRead() == 0; }
    bool full() const { return availableToWrite() == 0; }

    // Discard all readable items. Must not be called concurrently with
    // any other operation on this buffer.
    void clear() {
        if constexpr (Owning) {
            size_t rgh = readingHead.load(RELAXED);
            size_t wth = writtenHead.load(RELAXED);
            if constexpr (!is_trivially_copyable_v<T>) {
                for (size_t i = rgh; i < wth; i++) {
                    buffer[i % cap].~T();
                }
            }
        }
        size_t wth = writtenHead.load(RELAXED);
        readingHead.store(wth, RELAXED);
        readHead.store(wth, RELAXED);
    }
};

// Broadcast MPMC ring buffer where all registered consumers must read each item
// before its slot can be reused by producers.
//
// Wraps a SimpleMPMCRingBuffer for the write path and manages per-consumer read
// heads externally. A slot is only freed (global readHead advances) when every
// consumer has acknowledged it.
//
// Ack counters pack a 32-bit generation tag and 32-bit consumer count into a
// single Atomic<uint64_t>. The generation disambiguates reuse of physical slots
// across logical positions, eliminating the need for a separate reset step and
// avoiding a race between counter reset and slot reuse. 32-bit generation
// wraparound is handled correctly via unsigned modular equality — the maximum
// generation lag for any slot is 1, so aliasing cannot occur.
//
// Template parameters:
//   T      - element type
//   Owning - if true, buffer owns its backing storage
//
// Design note: per-consumer readHeads are single-writer (each headNumber is
// accessed by exactly one consumer thread). No CAS is needed for claiming
// read slots — a simple load + store suffices. The headNumber is expected to
// correspond to a logical consumer ID (e.g., CPU ID for TLB shootdowns).
template<typename T, bool Owning = true>
class BroadcastRingBuffer {
    SimpleMPMCRingBuffer<T, Owning, false> buffer;
    Atomic<size_t>* readHeads;      // one per consumer, single-writer
    Atomic<uint64_t>* ackCounters;  // one per physical slot, packs {gen:32, count:32}
    size_t consumerCount;
    size_t cap;

    static constexpr uint64_t packAck(uint32_t gen, uint32_t count) {
        return (static_cast<uint64_t>(gen) << 32) | count;
    }
    static constexpr uint32_t ackGen(uint64_t packed) {
        return static_cast<uint32_t>(packed >> 32);
    }
    static constexpr uint32_t ackCount(uint64_t packed) {
        return static_cast<uint32_t>(packed);
    }

    // Acknowledge that a consumer has finished reading logical position logicalPos.
    // If this consumer is the last to ack for this slot's current generation,
    // attempts to advance the global readHead.
    void ackSlot(size_t logicalPos) {
        size_t slot = logicalPos % cap;
        uint32_t expectedGen = static_cast<uint32_t>(logicalPos / cap);

        while (true) {
            uint64_t val = ackCounters[slot].load(ACQUIRE);
            uint32_t gen = ackGen(val);
            uint32_t count = ackCount(val);

            uint64_t newVal;
            if (gen == expectedGen) {
                // Same generation, increment count
                newVal = packAck(gen, count + 1);
            } else {
                // Old generation — we are the first consumer for this generation.
                // The previous generation must have been fully acked (count ==
                // consumerCount) for the slot to have been reused, so this CAS
                // implicitly resets the counter.
                newVal = packAck(expectedGen, 1);
            }

            if (ackCounters[slot].compare_exchange(val, newVal, RELEASE, ACQUIRE)) {
                if (ackCount(newVal) == static_cast<uint32_t>(consumerCount)) {
                    tryAdvanceReadHead();
                }
                break;
            }
        }
    }

    // Scan forward from the current global readHead, advancing past all
    // consecutive slots whose ack counters show full consumption.
    void tryAdvanceReadHead() {
        while (true) {
            size_t current = buffer.readHead.load(ACQUIRE);
            size_t slot = current % cap;
            uint64_t val = ackCounters[slot].load(ACQUIRE);
            uint32_t gen = ackGen(val);
            uint32_t count = ackCount(val);
            uint32_t expectedGen = static_cast<uint32_t>(current / cap);

            if (gen != expectedGen || count != static_cast<uint32_t>(consumerCount)) break;

            // All consumers have acked this slot — try to advance
            if (!buffer.readHead.compare_exchange(current, current + 1, RELEASE, RELAXED)) {
                continue; // Someone else advanced, retry from new position
            }
        }
    }

    // Non-copyable, non-movable
    BroadcastRingBuffer(const BroadcastRingBuffer&) = delete;
    BroadcastRingBuffer& operator=(const BroadcastRingBuffer&) = delete;

public:
    // Owning constructor: allocates internal storage
    BroadcastRingBuffer(size_t capacity, size_t consumerCount) requires (Owning)
        : buffer(capacity), consumerCount(consumerCount), cap(capacity) {
        readHeads = new Atomic<size_t>[consumerCount]();
        ackCounters = new Atomic<uint64_t>[capacity]();
    }

    // Non-owning constructor: borrows all external arrays
    BroadcastRingBuffer(T* buf, size_t capacity, size_t consumerCount,
                        Atomic<size_t>* readHeads, Atomic<uint64_t>* ackCounters) requires (!Owning)
        : buffer(buf, capacity), readHeads(readHeads), ackCounters(ackCounters),
          consumerCount(consumerCount), cap(capacity) {}

    ~BroadcastRingBuffer() {
        if constexpr (Owning) {
            delete[] readHeads;
            delete[] ackCounters;
        }
    }

    // --- Write interface: delegates directly to underlying buffer ---

    template<typename WriteCallback>
    bool bulkWrite(size_t count, WriteCallback callback) {
        return buffer.bulkWrite(count, forward<WriteCallback>(callback));
    }

    template<typename WriteCallback>
    bool tryBulkWrite(size_t count, WriteCallback callback) {
        return buffer.tryBulkWrite(count, forward<WriteCallback>(callback));
    }

    template<typename WriteCallback>
    size_t bulkWriteBestEffort(size_t count, WriteCallback callback) {
        return buffer.bulkWriteBestEffort(count, forward<WriteCallback>(callback));
    }

    // --- Read interface: per-consumer with broadcast acknowledgement ---
    //
    // Each headNumber must be used by exactly one consumer thread (single-writer).
    // The headNumber typically corresponds to a logical consumer ID (e.g., CPU ID).

    // All-or-nothing read for specific consumer.
    template<typename ReadCallback>
    bool bulkRead(size_t headNumber, size_t count, ReadCallback callback) {
        return tryBulkRead(headNumber, count, forward<ReadCallback>(callback));
    }

    // Non-blocking all-or-nothing read.
    // Returns true if count items were available, false otherwise.
    template<typename ReadCallback>
    bool tryBulkRead(size_t headNumber, size_t count, ReadCallback callback) {
        size_t myHead = readHeads[headNumber].load(RELAXED); // single-writer
        size_t wth = buffer.writtenHead.load(ACQUIRE);
        if (count > wth - myHead) return false;

        for (size_t i = 0; i < count; i++) {
            callback(i, static_cast<const T&>(buffer.buffer[(myHead + i) % cap]));
        }

        readHeads[headNumber].store(myHead + count, RELEASE);

        for (size_t i = 0; i < count; i++) {
            ackSlot(myHead + i);
        }
        return true;
    }

    // Best-effort: read up to count items, returns actual count read.
    template<typename ReadCallback>
    size_t bulkReadBestEffort(size_t headNumber, size_t count, ReadCallback callback) {
        size_t myHead = readHeads[headNumber].load(RELAXED); // single-writer
        size_t wth = buffer.writtenHead.load(ACQUIRE);
        size_t available = wth - myHead;
        size_t actualCount = min(count, available);
        if (actualCount == 0) return 0;

        for (size_t i = 0; i < actualCount; i++) {
            callback(i, static_cast<const T&>(buffer.buffer[(myHead + i) % cap]));
        }

        readHeads[headNumber].store(myHead + actualCount, RELEASE);

        for (size_t i = 0; i < actualCount; i++) {
            ackSlot(myHead + i);
        }
        return actualCount;
    }

    // --- Utility methods ---

    // Conservative estimate of items available for a specific consumer.
    size_t availableToRead(size_t headNumber) const {
        return buffer.writtenHead.load(ACQUIRE) - readHeads[headNumber].load(ACQUIRE);
    }

    size_t availableToWrite() const { return buffer.availableToWrite(); }
    bool empty() const { return buffer.empty(); }
    bool full() const { return buffer.full(); }

    // Discard all readable items across all consumers. Must not be called
    // concurrently with any other operation on this buffer. Ack counters
    // do not need explicit reset — the generation-based mechanism will
    // handle reinitialization when consumers next read.
    void clear() {
        size_t wth = buffer.writtenHead.load(RELAXED);
        for (size_t c = 0; c < consumerCount; c++) {
            readHeads[c].store(wth, RELAXED);
        }
        buffer.readHead.store(wth, RELAXED);
    }
};

#endif //CROCOS_RINGBUFFER_H
