//
// radix-tree Phase 0 — the oracle's state.
//
// Accounting and fault injection. The poison/unpoison half lives inline in the
// mock <mem/VMSubstrate.h>, where make/tryMake/destroy are.
//

#include "RadixOracle.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <assert_support.h>   // CroCOSTest::AssertionFailure

namespace {

constexpr size_t kMaxTypes = 32;

struct TypeEntry {
    const char* rawName = nullptr;   // __PRETTY_FUNCTION__ of typeIdOf<T>
    char        display[64] = {};
    size_t      bytes = 0;
};

std::mutex        gTypeMutex;
TypeEntry         gTypes[kMaxTypes];
std::atomic<size_t> gTypeCount{0};

// Counters are atomic and separated by type index. Not padded to cache lines:
// this is instrumentation, and the harness's own contention on them is not a
// property under test.
std::atomic<size_t> gLive[kMaxTypes];

// ─── Fault injection ───────────────────────────────────────────────────────

enum class Policy { None, All, ExactlyAt, AfterN };

std::mutex          gInjectMutex;
Policy              gPolicy = Policy::None;
size_t              gThreshold = 0;
std::atomic<size_t> gObserved{0};
std::atomic<size_t> gFailures{0};

// Turn "size_t kernel::mm::VMSubstrate::oracle::typeIdOf() [T = kernel::mm::Node32]"
// into "kernel::mm::Node32". Purely for the failure message; falls back to the
// raw string if the shape is not what we expect, because a mangled-but-present
// name still beats "type 3".
void makeDisplayName(const char* raw, char* out, size_t outSize) {
    const char* begin = std::strstr(raw, "T = ");
    if (!begin) {
        std::snprintf(out, outSize, "%s", raw);
        return;
    }
    begin += 4;
    const char* end = std::strrchr(begin, ']');
    if (!end || end <= begin) {
        std::snprintf(out, outSize, "%s", begin);
        return;
    }
    size_t n = static_cast<size_t>(end - begin);
    if (n >= outSize) n = outSize - 1;
    std::memcpy(out, begin, n);
    out[n] = '\0';
}

}  // namespace

namespace kernel::mm::VMSubstrate::oracle {

size_t registerType(const char* name, size_t bytes) {
    std::lock_guard<std::mutex> lock(gTypeMutex);
    const size_t id = gTypeCount.load(std::memory_order_relaxed);
    if (id >= kMaxTypes) {
        std::fprintf(stderr,
                     "RadixOracle: more than %zu distinct types registered; raise kMaxTypes\n",
                     kMaxTypes);
        std::abort();
    }
    gTypes[id].rawName = name;
    gTypes[id].bytes   = bytes;
    makeDisplayName(name, gTypes[id].display, sizeof(gTypes[id].display));
    gTypeCount.store(id + 1, std::memory_order_release);
    return id;
}

void noteConstructed(size_t typeId) {
    gLive[typeId].fetch_add(1, std::memory_order_relaxed);
}

void noteDestroyed(size_t typeId) {
    // Reported here rather than left to underflow into a huge number: a
    // destroy with no matching construct is the double-destroy half of the
    // rule, and it is much easier to read at the call site than as a live
    // count of 18446744073709551615 three tests later.
    const size_t prior = gLive[typeId].fetch_sub(1, std::memory_order_relaxed);
    if (prior == 0) {
        std::fprintf(stderr, "RadixOracle: destroy of '%s' with live count already 0 "
                             "(double destroy)\n", gTypes[typeId].display);
        std::abort();
    }
}

bool shouldInjectFailure() {
    const size_t n = gObserved.fetch_add(1, std::memory_order_relaxed);
    Policy policy;
    size_t threshold;
    {
        std::lock_guard<std::mutex> lock(gInjectMutex);
        policy    = gPolicy;
        threshold = gThreshold;
    }
    bool fail = false;
    switch (policy) {
        case Policy::None:      fail = false;           break;
        case Policy::All:       fail = true;            break;
        case Policy::ExactlyAt: fail = (n == threshold); break;
        case Policy::AfterN:    fail = (n >= threshold); break;
    }
    if (fail) gFailures.fetch_add(1, std::memory_order_relaxed);
    return fail;
}

}  // namespace kernel::mm::VMSubstrate::oracle

namespace CroCOSTest::radix {

size_t liveCount(size_t typeId) {
    return gLive[typeId].load(std::memory_order_relaxed);
}

size_t totalLive() {
    size_t sum = 0;
    const size_t n = gTypeCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; i++) sum += gLive[i].load(std::memory_order_relaxed);
    return sum;
}

size_t registeredTypeCount() { return gTypeCount.load(std::memory_order_acquire); }

const char* typeName(size_t typeId) {
    return (typeId < kMaxTypes && gTypes[typeId].rawName) ? gTypes[typeId].display : "<unregistered>";
}

size_t typeSize(size_t typeId) {
    return (typeId < kMaxTypes) ? gTypes[typeId].bytes : 0;
}

Baseline captureBaseline() {
    Baseline b{};
    b.types = gTypeCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < b.types && i < kMaxTypes; i++) {
        b.counts[i] = gLive[i].load(std::memory_order_relaxed);
    }
    return b;
}

void assertBaseline(const Baseline& b, const char* what) {
    // Compare over the CURRENT type count, not the snapshot's: a type first
    // registered after the snapshot has an implicit baseline of zero, and
    // objects of it leaking is exactly the case a snapshot-width loop would
    // miss.
    const size_t n = gTypeCount.load(std::memory_order_acquire);
    char message[512];
    size_t used = 0;
    bool  differs = false;

    for (size_t i = 0; i < n && i < kMaxTypes; i++) {
        const size_t before = (i < b.types) ? b.counts[i] : 0;
        const size_t now    = gLive[i].load(std::memory_order_relaxed);
        if (now == before) continue;
        differs = true;
        if (used < sizeof(message) - 1) {
            const long long delta = static_cast<long long>(now) - static_cast<long long>(before);
            used += static_cast<size_t>(std::snprintf(
                message + used, sizeof(message) - used,
                "%s%s: %zu -> %zu (%+lld, %zu B each)",
                used ? "; " : "", typeName(i), before, now, delta, typeSize(i)));
        }
    }

    if (differs) {
        char full[640];
        std::snprintf(full, sizeof(full),
                      "%s: live-object accounting did not return to baseline — %s",
                      what, message);
        throw CroCOSTest::AssertionFailure(std::string(full));
    }
}

void assertNoLiveObjects(const char* what) {
    Baseline zero{};
    zero.types = 0;   // every type's implied baseline is 0
    assertBaseline(zero, what);
}

void resetAccounting() {
    const size_t n = gTypeCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < kMaxTypes; i++) {
        gLive[i].store(0, std::memory_order_relaxed);
    }
}

void injectNothing() {
    std::lock_guard<std::mutex> lock(gInjectMutex);
    gPolicy = Policy::None;
    gThreshold = 0;
    gObserved.store(0, std::memory_order_relaxed);
    gFailures.store(0, std::memory_order_relaxed);
}

void injectAllFailures() {
    std::lock_guard<std::mutex> lock(gInjectMutex);
    gPolicy = Policy::All;
    gObserved.store(0, std::memory_order_relaxed);
    gFailures.store(0, std::memory_order_relaxed);
}

void injectFailureAt(size_t n) {
    std::lock_guard<std::mutex> lock(gInjectMutex);
    gPolicy = Policy::ExactlyAt;
    gThreshold = n;
    gObserved.store(0, std::memory_order_relaxed);
    gFailures.store(0, std::memory_order_relaxed);
}

void injectFailuresAfter(size_t n) {
    std::lock_guard<std::mutex> lock(gInjectMutex);
    gPolicy = Policy::AfterN;
    gThreshold = n;
    gObserved.store(0, std::memory_order_relaxed);
    gFailures.store(0, std::memory_order_relaxed);
}

void resetInjection() { injectNothing(); }

size_t injectionObservedCalls() { return gObserved.load(std::memory_order_relaxed); }
size_t injectionFailureCount()  { return gFailures.load(std::memory_order_relaxed); }

}  // namespace CroCOSTest::radix
