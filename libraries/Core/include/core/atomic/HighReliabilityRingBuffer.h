#ifndef CROCOS_HIGH_RELIABILITY_RING_BUFFER_H
#define CROCOS_HIGH_RELIABILITY_RING_BUFFER_H

#include <core/atomic/RingBuffer.h>
#include <core/atomic.h>

// Wrapper around MPMCRingBuffer that provides guaranteed eventual delivery
// for reads by maintaining a separate logical count of available items.
//
// Problem: under ScanOnComplete=true, writers increment writeHead, write data,
// store gen counters (SEQ_CST), then advance writtenHead. Between the gen counter
// store and writtenHead advancing, an item is logically present but invisible to
// readers checking writtenHead. This causes false read failures.
//
// Solution: logicalCount is incremented eagerly before the underlying write and
// decremented when a reader claims an item. If the buffer read then stalls (write
// not yet visible via writtenHead), the reader spins until the write completes.
//
// Same structural overflow contract as MPMCRingBuffer: callers must guarantee
// that the total number of items in flight never exceeds capacity.
template<typename T, bool Owning = true, bool ScanOnComplete = false>
class HighReliabilityRingBuffer {
    MPMCRingBuffer<T, Owning, ScanOnComplete> buffer;
    Atomic<size_t> logicalCount{size_t(0)};

    // Drain `toClaim` already-claimed items from the underlying buffer, invoking
    // callback(callbackOffset + i, slot) for each. Spins if writtenHead has not
    // yet caught up. Returns number actually drained; restores any un-drained
    // items to logicalCount on spin timeout.
    template<typename ReadCallback>
    size_t drainClaimed(size_t toClaim, size_t callbackOffset,
                        ReadCallback& callback, size_t maxSpins) {
        size_t drained = 0;
        size_t spinsWithoutProgress = 0;
        while (drained < toClaim) {
            size_t got = buffer.bulkReadBestEffort(toClaim - drained,
                [&](size_t i, const T& slot) {
                    callback(callbackOffset + drained + i, slot);
                });
            if (got > 0) {
                drained += got;
                spinsWithoutProgress = 0;
            } else {
                if (++spinsWithoutProgress > maxSpins) {
                    logicalCount.fetch_add(toClaim - drained, RELEASE);
                    return drained;
                }
                tight_spin();
            }
        }
        return drained;
    }

public:
    static constexpr size_t DEFAULT_MAX_SPINS = 10000;

    // Owning constructor
    explicit HighReliabilityRingBuffer(size_t capacity) requires (Owning)
        : buffer(capacity) {}

    // Non-owning, no gen counters
    HighReliabilityRingBuffer(T* buf, size_t capacity)
        requires (!Owning && !ScanOnComplete)
        : buffer(buf, capacity) {}

    // Non-owning, with write gen counters (ScanOnComplete=true)
    HighReliabilityRingBuffer(T* buf, size_t capacity, Atomic<size_t>* wgc, Atomic<size_t>* rgc)
        requires (!Owning && ScanOnComplete)
        : buffer(buf, capacity, wgc, rgc) {}

    HighReliabilityRingBuffer(const HighReliabilityRingBuffer&) = delete;
    HighReliabilityRingBuffer& operator=(const HighReliabilityRingBuffer&) = delete;

    // --- Write ---

    template<typename WriteCallback>
    bool bulkWrite(size_t count, WriteCallback callback) {
        logicalCount.fetch_add(count, RELEASE);
        if (!buffer.bulkWrite(count, forward<WriteCallback>(callback))) {
            logicalCount.fetch_sub(count, RELEASE);
            return false;
        }
        return true;
    }

    // Increments logicalCount AFTER the write so readers never need to spin
    // for items from this path (writtenHead is already advanced by the time
    // logicalCount reflects them).
    template<typename WriteCallback>
    size_t bulkWriteBestEffort(size_t count, WriteCallback callback) {
        size_t actual = buffer.bulkWriteBestEffort(count, forward<WriteCallback>(callback));
        if (actual > 0) logicalCount.fetch_add(actual, RELEASE);
        return actual;
    }

    bool write(const T& value) {
        return bulkWrite(1, [&](size_t, T& slot) { slot = value; });
    }

    // --- Read ---

    // Best-effort: reads up to maxCount items. CAS-claims as many as available
    // per iteration, draining each batch via the spin loop. Exits when
    // logicalCount reaches 0 or maxCount is satisfied.
    template<typename ReadCallback>
    size_t bulkReadBestEffort(size_t maxCount, ReadCallback callback,
                              size_t maxSpins = DEFAULT_MAX_SPINS) {
        size_t consumed = 0;
        while (consumed < maxCount) {
            size_t expected = logicalCount.load(ACQUIRE);
            if (expected == 0) break;
            size_t toClaim = min(expected, maxCount - consumed);
            if (!logicalCount.compare_exchange(expected, expected - toClaim, ACQ_REL, ACQUIRE))
                continue;
            consumed += drainClaimed(toClaim, consumed, callback, maxSpins);
        }
        return consumed;
    }

    // All-or-nothing: claims exactly count items atomically, then drains them.
    // Returns false (without reading) if fewer than count items are available.
    template<typename ReadCallback>
    bool bulkRead(size_t count, ReadCallback callback,
                  size_t maxSpins = DEFAULT_MAX_SPINS) {
        size_t expected = logicalCount.load(ACQUIRE);
        while (true) {
            if (expected < count) return false;
            if (logicalCount.compare_exchange(expected, expected - count, ACQ_REL, ACQUIRE))
                break;
        }
        return drainClaimed(count, 0, callback, maxSpins) == count;
    }

    bool tryRead(T& out, size_t maxSpins = DEFAULT_MAX_SPINS) {
        return bulkReadBestEffort(1, [&](size_t, const T& slot) { out = slot; }, maxSpins) == 1;
    }

    // --- Utility ---

    size_t availableToRead() const { return logicalCount.load(ACQUIRE); }
    size_t availableToWrite() const { return buffer.availableToWrite(); }
    bool empty() const { return logicalCount.load(ACQUIRE) == 0; }
    bool full() const { return buffer.full(); }
    void clear() { buffer.clear(); logicalCount.store(0, RELEASE); }
};

#endif // CROCOS_HIGH_RELIABILITY_RING_BUFFER_H