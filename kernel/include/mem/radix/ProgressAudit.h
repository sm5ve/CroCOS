//
// radix-tree — §6.7's maximum-holder lemma, made assertable (§11's progress row).
//
// §11 states the ONLY assertable progress property, and states it narrowly
// because three plausible neighbours are false:
//
//   "the holder of the lexicographically maximum writer-held site never fails
//    against a bit another WRITER holds" (§6.8's order)
//
// and immediately fences it: "failures against a terminal mask or a transient
// phantom are legal, bounded-retry events and get counters, not asserts. It must
// NOT be stated as 'never fails an acquisition' — that is false against a
// transient phantom and fires on a legal execution. Do not restore either
// earlier formulation: the per-round one and the deepest-claim-holder one are
// both false, and the second fires on two ordinary adjacent munmaps."
//
// ─── Why this needs a registry at all ──────────────────────────────────────
//
// The assert has to classify a failed acquisition into one of three buckets, and
// the state word alone cannot do it. §6.7 enumerates them exactly:
//
//   - a **writer-held** claim — the only one that may be asserted on;
//   - the **terminal mask** of a marked node, `valenceMask | MARK`, left behind
//     until the grace period ends — visible in the word, so cheap to exclude;
//   - a **transient phantom** — bits a loser set between its `fetch_or` and its
//     `mask & ~prior` clear, which NOBODY holds and which make a genuinely free
//     site fail. Indistinguishable from a held bit by inspection: same bit, same
//     word, and the only difference is whether some writer currently considers
//     itself the owner.
//
// So the classification is a question about other writers' *intent*, and the
// only place that lives is the writers. Hence: each writer publishes what it
// holds, and a failing writer asks.
//
// ─── Why it is sound despite being lock-free ───────────────────────────────
//
// A naive registry read races: a writer can acquire or release between our
// `fetch_or` and our query, so "W is registered now" does not mean "W held it
// when we failed" — and concluding the wrong way would fire the assert on a
// legal execution, which is the one outcome §11 spends four sentences warning
// against.
//
// The fix is a seqlock-shaped window rather than a lock. Every writer bumps its
// epoch to odd before mutating its published set and to even after. A failing
// writer samples every other writer's epoch BEFORE its `fetch_or` and again
// after the failure; if an epoch is unchanged and even across that window, that
// writer's held set provably did not move while we failed, so reading it is
// exact. If ANY epoch moved, we learn nothing and count the event instead of
// asserting — deliberately biased toward silence, because a missed violation
// costs a test that could have been sharper while a false one costs a day.
//
// The cost on the common path is one relaxed load per CPU, not a scan.
//
// ─── Off by default ────────────────────────────────────────────────────────
//
// Publishing is two epoch bumps and two stores per claim acquired, on the
// hottest path in the protocol. It is a verification instrument, not a
// production one, so it compiles to nothing unless CROCOS_RADIX_PROGRESS_AUDIT
// is defined.
//

#ifndef CROCOS_RADIX_PROGRESS_AUDIT_H
#define CROCOS_RADIX_PROGRESS_AUDIT_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <core/atomic.h>
#include <arch/amd64/amd64.h>

#include <mem/radix/Node.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    // A site in §6.8's total order: (level, then ascending VA). "Depth is a
    // node's level in the CONCEPTUAL FULL-DEPTH TREE, derived from the slot
    // span" — the only formulation invariant under cluster growth.
    struct AuditSite {
        bool     valid = false;
        unsigned level = 0;
        uint64_t base  = 0;

        [[nodiscard]] bool greaterThan(const AuditSite& o) const {
            if (!o.valid) return valid;
            if (!valid) return false;
            if (level != o.level) return level > o.level;
            return base > o.base;
        }
    };

#ifdef CROCOS_RADIX_PROGRESS_AUDIT

    // Per-CPU published held set. Sized for the largest claim set any geometry
    // builds; the audit is debug-only, so the array is sized generously rather
    // than tuned.
    inline constexpr size_t kAuditMaxHeld = 128;

    struct WriterAuditCell {
        // Even = stable, odd = mid-update. A reader that sees the same EVEN
        // value before and after its window knows the set below did not move.
        Atomic<uint64_t> epoch{0};
        Atomic<uint64_t> count{0};
        Atomic<uint64_t> maxLevel{0};
        Atomic<uint64_t> maxBase{0};
        Atomic<uint64_t> maxValid{0};
        Atomic<void*>    node[kAuditMaxHeld];
        Atomic<uint64_t> mask[kAuditMaxHeld];
    };

    // One row per CPU. Deliberately a flat global rather than per-tree: a writer
    // holds claims in exactly one tree at a time, and keying by CPU is what makes
    // "every OTHER writer" a bounded scan.
    inline WriterAuditCell gWriterAudit[arch::MAX_PROCESSOR_COUNT];

    // Counters for the two legal failure shapes, which §11 requires be counted
    // rather than asserted.
    struct ProgressAuditCounters {
        Atomic<uint64_t> terminalMaskFailures{0};
        Atomic<uint64_t> phantomFailures{0};
        Atomic<uint64_t> writerHeldFailures{0};
        Atomic<uint64_t> inconclusive{0};
    };
    inline ProgressAuditCounters gProgressAuditCounters;

    inline void auditReset() {
        for (size_t c = 0; c < arch::MAX_PROCESSOR_COUNT; c++) {
            gWriterAudit[c].epoch.store(0, kPrivateInit);
            gWriterAudit[c].count.store(0, kPrivateInit);
            gWriterAudit[c].maxValid.store(0, kPrivateInit);
        }
        gProgressAuditCounters.terminalMaskFailures.store(0, kPrivateInit);
        gProgressAuditCounters.phantomFailures.store(0, kPrivateInit);
        gProgressAuditCounters.writerHeldFailures.store(0, kPrivateInit);
        gProgressAuditCounters.inconclusive.store(0, kPrivateInit);
    }

    inline WriterAuditCell& auditSelf() {
        return gWriterAudit[arch::getCurrentProcessorID()];
    }

    // Republish this writer's held set. Called after every acquisition and every
    // release, bracketed by the odd/even epoch so a concurrent reader can tell
    // whether it caught us mid-update.
    template <typename ClaimSetT>
    inline void auditPublish(const ClaimSetT& set) {
        WriterAuditCell& me = auditSelf();
        const uint64_t e = me.epoch.load(kQuiescedRead);
        me.epoch.store(e | 1, kRefcountRelease);          // odd: mid-update

        size_t    n = 0;
        AuditSite max;
        for (size_t i = 0; i < set.count && n < kAuditMaxHeld; i++) {
            const auto& entry = set.entries[i];
            if (!entry.held) continue;
            me.node[n].store(entry.node.raw(), kRefcountRelease);
            me.mask[n].store(entry.mask, kRefcountRelease);
            n++;
            const AuditSite s{true, entry.level, entry.nodeBase};
            if (s.greaterThan(max)) max = s;
        }
        me.count.store(n, kRefcountRelease);
        me.maxLevel.store(max.level, kRefcountRelease);
        me.maxBase.store(max.base, kRefcountRelease);
        me.maxValid.store(max.valid ? 1 : 0, kRefcountRelease);

        me.epoch.store((e | 1) + 1, kRefcountRelease);    // even: stable again
    }

    // Sample every other writer's epoch. Taken BEFORE the fetch_or, so a later
    // comparison can establish that a writer's set spanned our failure.
    struct AuditEpochSnapshot {
        uint64_t epoch[arch::MAX_PROCESSOR_COUNT];
    };

    inline void auditSnapshotEpochs(AuditEpochSnapshot& s) {
        for (size_t c = 0; c < arch::MAX_PROCESSOR_COUNT; c++) {
            s.epoch[c] = gWriterAudit[c].epoch.load(kQuiescedRead);
        }
    }

    // The assert itself. `prior` is the word our fetch_or returned, `mask` what
    // we asked for, `node` where, and `before` the epochs sampled ahead of it.
    // `selfMax` is our own maximum HELD site at the moment of failure.
    inline void auditFailedAcquisition(const AuditEpochSnapshot& before,
                                       NodeRef node, uint64_t mask, uint64_t prior,
                                       const AuditSite& selfMax, const AuditSite& site) {
        // §6.7: the terminal mask of a marked node. Legal, self-limiting as the
        // unlink store propagates, and explicitly NOT a lemma counterexample —
        // "no active writer holds it", so folding it in would make the lemma
        // vacuous exactly where it needs to bind.
        if ((prior & state::kMarkMask) != 0) {
            gProgressAuditCounters.terminalMaskFailures.fetch_add(1, kRefcountAcquire);
            return;
        }

        const uint64_t failing = prior & mask;
        if (failing == 0) return;                 // not a claim conflict at all

        const unsigned self = arch::getCurrentProcessorID();
        bool     writerHeld = false;
        bool     conclusive = true;
        AuditSite globalMax = selfMax;

        for (size_t c = 0; c < arch::MAX_PROCESSOR_COUNT; c++) {
            if (c == self) continue;
            const uint64_t now = gWriterAudit[c].epoch.load(kQuiescedRead);
            // Unchanged AND even means this writer's published set did not move
            // across our fetch_or, so reading it below is exact rather than a
            // guess about a racing peer.
            if (now != before.epoch[c] || (now & 1) != 0) {
                conclusive = false;
                continue;
            }
            const uint64_t n = gWriterAudit[c].count.load(kQuiescedRead);
            for (uint64_t i = 0; i < n && i < kAuditMaxHeld; i++) {
                if (gWriterAudit[c].node[i].load(kQuiescedRead) != node.raw()) continue;
                if ((gWriterAudit[c].mask[i].load(kQuiescedRead) & failing) != 0) {
                    writerHeld = true;
                }
            }
            if (gWriterAudit[c].maxValid.load(kQuiescedRead) != 0) {
                const AuditSite s{true,
                                  static_cast<unsigned>(
                                      gWriterAudit[c].maxLevel.load(kQuiescedRead)),
                                  gWriterAudit[c].maxBase.load(kQuiescedRead)};
                if (s.greaterThan(globalMax)) globalMax = s;
            }
        }

        if (!writerHeld) {
            // §6.7's transient phantom: "bits a loser has set between its
            // fetch_or and its mask & ~prior clear, which nobody holds and which
            // make a genuinely free site fail." The lemma's boundary, and no
            // argument crosses it — a counter, never an assert.
            gProgressAuditCounters.phantomFailures.fetch_add(1, kRefcountAcquire);
            return;
        }

        gProgressAuditCounters.writerHeldFailures.fetch_add(1, kRefcountAcquire);

        if (!conclusive) {
            // Some peer moved across our window, so we cannot tell whether we
            // were the maximum holder at the instant we failed. Biased to
            // silence on purpose.
            gProgressAuditCounters.inconclusive.fetch_add(1, kRefcountAcquire);
            return;
        }

        // We failed against a bit a writer holds. The lemma forbids that for the
        // holder of the lexicographically maximum writer-held site, and its proof
        // names the hypothesis explicitly: "A FAILURE AT A GREATER SITE would
        // require a live holder at a greater site, contradicting maximality."
        //
        // Both halves are required, and the second is easy to drop by accident:
        //
        //   selfMax is the global maximum   — we are the max holder;
        //   site > selfMax                  — and this acquisition is at a site
        //                                     greater than everything held.
        //
        // §6.8's ascending order is what makes the second half hold on every
        // acquisition, which is why §6.7 calls the order "load-bearing for
        // DEADLOCK-FREEDOM, not merely for the mark's irreversibility". Leaving it
        // implicit would turn this into an assert about a hypothesis that no
        // longer holds the moment the order is broken — and an order violation is
        // a different defect, caught deterministically by the order assert in
        // `acquireAll` rather than probabilistically here.
        const bool weAreMaxHolder    = selfMax.valid && !globalMax.greaterThan(selfMax);
        const bool acquiringAbove    = site.greaterThan(selfMax);
        assert(!(weAreMaxHolder && acquiringAbove),
               "radix: the holder of the lexicographically maximum writer-held site failed "
               "against a bit another writer holds, at a site greater than every held site "
               "— §6.7's maximum-holder lemma, which is what deadlock-freedom rests on");
    }

#else   // !CROCOS_RADIX_PROGRESS_AUDIT

    struct AuditEpochSnapshot {};
    template <typename ClaimSetT> inline void auditPublish(const ClaimSetT&) {}
    inline void auditSnapshotEpochs(AuditEpochSnapshot&) {}
    inline void auditFailedAcquisition(const AuditEpochSnapshot&, NodeRef, uint64_t,
                                       uint64_t, const AuditSite&, const AuditSite&) {}
    inline void auditReset() {}

#endif

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_PROGRESS_AUDIT_H
