//
// Freshness accounting for the radix harness (D-049).
//
// The one thing the userspace mock can say about `ensureTLBEntryFresh` that is
// worth saying: **it was called, on this thread, naming this page.** Userspace
// has no TLB, so the call's return value is a foregone "not stale" and every
// test passes whether the call is there or not — which is why all eight members
// of this bug class so far were found by an in-kernel stress boot rather than by
// the 156-test suite that runs on two sanitizers.
//
// Three decisions, each of which the obvious alternative gets wrong:
//
//   * **Thread-local, not global.** Freshness is a property of ONE CPU's
//     mapping — that is the entire content of D-044's ruling — so "this thread
//     discharged freshness for this page" is the proposition worth asserting. A
//     global record answers a weaker one ("somebody did"), which is exactly the
//     weaker claim that let a `Mapping` acquired on one CPU be read on another.
//     It also keeps every SafePtr access in the torture suite contention-free.
//
//   * **Armed explicitly, disarmed by default.** Disarmed this costs one
//     thread-local bool test per access, so the concurrent, torture and soak
//     runs are unperturbed and nobody has to reason about whether the
//     instrumentation changed a race's timing.
//
//   * **A fixed array, not a set.** No allocation on the recording path, so the
//     harness's per-test "N bytes allocated, N bytes freed" accounting keeps
//     meaning what it says, and a thread-local container cannot look like a
//     leak. The cost is a bound, and the bound is reported rather than hidden:
//     `freshnessRecordOverflowed()` exists so a test can never pass on a
//     truncated record — the only way this could lie in the direction that
//     matters.
//

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <mem/VMSubstrate.h>

namespace kernel::mm::VMSubstrate::test {

    namespace {
        // Distinct pages one thread can name between two clears. Sized for the
        // shape these assertions have — a handful of nodes plus a record plus a
        // bucket page — not for a whole descent, and overflow is visible.
        constexpr size_t kMaxRecordedPages = 128;

        thread_local uintptr_t gPages[kMaxRecordedPages] = {};
        thread_local size_t    gPageCount = 0;
        thread_local uint64_t  gCalls     = 0;
        thread_local bool      gOverflow  = false;

        uintptr_t pageOf(const void* p) {
            return reinterpret_cast<uintptr_t>(p) & ~(static_cast<uintptr_t>(arch::smallPageSize) - 1);
        }
    }

    thread_local bool freshnessRecordingArmed = false;

    void recordFreshnessCall(const void* p) {
        gCalls++;
        if (p == nullptr) return;              // a null SafePtr deref is a bug elsewhere
        const uintptr_t page = pageOf(p);
        for (size_t i = 0; i < gPageCount; i++) {
            if (gPages[i] == page) return;
        }
        if (gPageCount == kMaxRecordedPages) { gOverflow = true; return; }
        gPages[gPageCount++] = page;
    }

    void armFreshnessRecording() {
        clearFreshnessRecord();
        freshnessRecordingArmed = true;
    }

    void disarmFreshnessRecording() {
        freshnessRecordingArmed = false;
        clearFreshnessRecord();
    }

    void clearFreshnessRecord() {
        gPageCount = 0;
        gCalls     = 0;
        gOverflow  = false;
    }

    uint64_t freshnessCalls() { return gCalls; }

    bool pageWasMadeFresh(const void* p) {
        if (p == nullptr) return false;
        const uintptr_t page = pageOf(p);
        for (size_t i = 0; i < gPageCount; i++) {
            if (gPages[i] == page) return true;
        }
        return false;
    }

    bool freshnessRecordOverflowed() { return gOverflow; }

}   // namespace kernel::mm::VMSubstrate::test
