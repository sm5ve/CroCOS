//
// Preemption and CPU-affinity control — the seam a scheduler will fill.
//
// **There is no scheduler yet.** Every operation here is a no-op and every
// predicate returns true, because a CPU today runs one thing until it is done.
// This header exists so that the code which *depends* on that fact says so, at
// the point where it depends on it, in a vocabulary a scheduler can implement
// later without hunting for the sites.
//
// ─── Two obligations, not one ──────────────────────────────────────────────
//
// They are independent and are separated deliberately, because the code that
// needs them needs different ones:
//
//   * **Preemption disabled** — no other thread runs on this CPU until the
//     guard drops. What a lock-free per-CPU structure needs when its safety
//     argument says "only one actor pops".
//   * **CPU pinned** — this thread does not migrate to another CPU. What
//     per-CPU *identity* needs: a value read from CPU N must still describe the
//     CPU we are on when we use it.
//
// Disabling preemption implies pinning (nothing can move a thread that is not
// scheduled away), but pinning does NOT imply preemption is off — a thread can
// be preempted and resumed on the same CPU. `kernel::rcu`'s `barrier` wants
// exactly the weaker one: RCU-DEC-040 makes its guarantee per-SLOT, so the
// caller must hold affinity from retire through barrier while preemption
// itself "stays fine".
//
// ─── Why this is a subsystem rather than three copies ──────────────────────
//
// It was three copies. `vmsmalloc.cpp` and `kernel/rcu/RCU.cpp` each carried a
// private `preemptionDisabled()`/`cpuPinned()` pair, duplicated on the reasoning
// that they are vacuous today and asserting them anyway means "the assertions
// become real when a scheduler lands, without anyone having to remember to add
// them". The reasoning is right and the duplication worked against it: with the
// radix tree a third consumer would have meant three places to remember. They
// now delegate here, so a scheduler implements this file and every existing
// assertion becomes real at once.
//
// ─── Implementing this later ───────────────────────────────────────────────
//
// The intended shape, when a scheduler exists:
//
//   * a preempt-count in `CpuLocal` (per-CPU, incremented by the guard, tested
//     by the scheduler's preemption point);
//   * a migrate-disable count per thread, in whatever the thread structure is;
//   * `preemptionDisabled()`/`cpuPinned()` read those counts;
//   * the guards become the counted increment/decrement.
//
// The counts must be nesting-safe — every consumer below nests them under other
// locks and sections — which is why the guards are counted rather than boolean
// and why they are RAII rather than paired calls.
//

#ifndef CROCOS_SCHED_PREEMPTION_H
#define CROCOS_SCHED_PREEMPTION_H

namespace kernel::sched {

    // ─── Predicates, for assertions ────────────────────────────────────────
    //
    // Debug-only checks per the project's safety stance: trust the caller in
    // release, catch the mistake in debug. Vacuously true today.

    // No other thread will run on this CPU until preemption is re-enabled.
    [[nodiscard]] inline bool preemptionDisabled() noexcept {
        // TODO(future scheduler): per-CPU preempt-count check.
        return true;
    }

    // This thread will not migrate to another CPU.
    [[nodiscard]] inline bool cpuPinned() noexcept {
        // TODO(future scheduler): per-thread migrate-disable check.
        return true;
    }

    // ─── Guards ────────────────────────────────────────────────────────────
    //
    // RAII rather than paired calls, so an early return cannot leak a disabled
    // preempt count — which under a real scheduler is a hung CPU, not a leak.

    // Disables preemption for the scope. Implies `CpuPinGuard`.
    class PreemptionGuard {
    public:
        PreemptionGuard() noexcept {
            // TODO(future scheduler): ++cpuLocal().preemptCount;
        }
        ~PreemptionGuard() {
            // TODO(future scheduler): --cpuLocal().preemptCount, and take the
            // preemption point if it reached zero and one is pending.
        }

        PreemptionGuard(const PreemptionGuard&)            = delete;
        PreemptionGuard& operator=(const PreemptionGuard&) = delete;
        PreemptionGuard(PreemptionGuard&&)                 = delete;
        PreemptionGuard& operator=(PreemptionGuard&&)      = delete;
    };

    // Pins this thread to its current CPU for the scope, without disabling
    // preemption. The weaker of the two — use it where per-CPU *identity* must
    // hold but another thread running here is harmless.
    class CpuPinGuard {
    public:
        CpuPinGuard() noexcept {
            // TODO(future scheduler): ++currentThread().migrateDisableCount;
        }
        ~CpuPinGuard() {
            // TODO(future scheduler): --currentThread().migrateDisableCount.
        }

        CpuPinGuard(const CpuPinGuard&)            = delete;
        CpuPinGuard& operator=(const CpuPinGuard&) = delete;
        CpuPinGuard(CpuPinGuard&&)                 = delete;
        CpuPinGuard& operator=(CpuPinGuard&&)      = delete;
    };

}  // namespace kernel::sched

#endif  // CROCOS_SCHED_PREEMPTION_H
