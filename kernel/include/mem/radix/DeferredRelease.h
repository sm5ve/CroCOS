//
// radix-tree — DEC-068's deferred `Mapping` releases and their per-CPU
// recycling pools (§7.1).
//
// ─── Why a record at all ──────────────────────────────────────────────────
//
// §7.1's composition rule is that **every release of a structural reference on
// a published node is deferred**, and §10 names the alternative as the trap:
// "a synchronous release on a published node is a use-after-free with no grace
// period at all — and it is the NATURAL implementation", because the commit
// walk already visits every slot, so releasing there costs nothing and looks
// like tidiness. A reader inside the section that observed the old slot word
// takes its counted reference after the writer has taken the count to zero.
//
// Nodes defer for free: a node is unlinked exactly once, so its own retire
// carries the releases its slots hold. A `Mapping` cannot do that. **The
// relationship is N:1**, so two CPUs clearing two slots that name the same
// record would enqueue the same intrusive linkage twice — a lost release
// (permanent leak) or a duplicated one (the count reaches zero with a live
// naming slot, and the next fault dereferences freed memory through §5.4's
// hot-path step). A single-threaded `munmap` of a three-slot region hits it
// deterministically. So the release rides its own retire subject.
//
// ─── Why the records are POOLED and never allocated on the path ───────────
//
// Allocating here would make `munmap` an allocating operation — "the one
// operation that RELIEVES memory pressure the one that fails under it"
// (DEC-068). The pool removes the question rather than answering it, and it is
// why the deleter PUSHES A RECORD BACK rather than freeing it: an earlier
// "the deleter both releases and frees" rule re-created the hazard one step
// removed, since every record consumed had to be re-allocated on some later
// operation's way in.
//
// The population is fixed at address-space creation, one `tryMake` per record
// (a single ~230-record array would exceed vmsmalloc's one-page object ceiling,
// and the records are independent retire subjects in any case). So it is
// address-space *creation* that fails cleanly under pressure.
//
// ─── The shortfall path, and why it cannot be a wait in place ─────────────
//
// A drawing CPU that finds its pool short is seeing records in flight through
// its own earlier retires, not a leak — the pool is per-CPU and only its owner
// draws from it. But the remedy is a blocking call, and the shortfall is
// discovered *inside the attempt's section*, so the attempt must end first:
// waiting on a grace period from inside one's own read section waits on itself
// forever (§10's DEC-046 genre of self-deadlock).
//
// The replenish primitive is **`barrier`, never `synchronize`** — a distinction
// that reads like a detail and is not. `synchronize` promises only that a grace
// period has elapsed and **does not seal the caller's still-open bag**; the
// records this CPU needs are precisely the ones in that bag. `barrier` seals,
// rotates and drives the owner's retirees, whose deleters push every missing
// record home, and since only the owner draws from its own pool the retried
// attempt's draw then succeeds deterministically.
//
// ─── Freshness ───────────────────────────────────────────────────────────
//
// RCU's `onPreTouch` covers a retire subject's `RetireHead` and nothing else,
// so a deleter's accesses to the rest of the record body are cross-CPU accesses
// to vmsmalloc-backed memory and need the `ensureTLBEntryFresh` discipline —
// the DEC-047 stale-TLB bug class. **The same applies to each record's first
// draw**: records are allocated by the CREATING CPU, so the owning CPU's first
// draw is a cross-CPU first touch of possibly-recycled arena memory, with no
// deleter in sight. Paid at the draw site.
//
// The pool HEADS are exempt: they live in pinned per-address-space storage
// whose mapping never changes.
//

#ifndef CROCOS_RADIX_DEFERRED_RELEASE_H
#define CROCOS_RADIX_DEFERRED_RELEASE_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <kassert.h>
#include <core/atomic.h>
#include <core/rcu/EpochDomain.h>
#include <mem/VMSubstrate.h>
#include <rcu/RCU.h>

#include <mem/radix/Geometry.h>
#include <mem/radix/Mapping.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    struct DeferredReleasePool;

    struct DeferredRelease {
        // The record's payload: which `Mapping`, and how much. `delta` is an
        // AGGREGATE — §7.1 asks for one record per DISTINCT `Mapping` that stops
        // being named via a directly-written slot, carrying the whole deferred
        // decrement, which is why this is a count and not a flag. The typical
        // shape is what makes it worth the dedup: a `munmap` over fifteen slots
        // that all name one record draws ONE record with delta 15.
        Mapping* mapping = nullptr;
        uint64_t delta   = 0;

        // The pool this record must return to. Named on the record rather than
        // recomputed at the deleter, because a deleter runs wherever a pump runs
        // — "the CPU that drains it" and "the CPU that owns it" are different
        // CPUs in the ordinary case.
        DeferredReleasePool* homePool = nullptr;

        // Two lives, never overlapping, so one field serves both:
        //   * while the record sits in a pool, this is the Treiber stack link;
        //   * while an attempt holds a drawn record, this threads the attempt's
        //     held list, which is what keeps the held set off the stack — the
        //     alternative is a ~230-pointer array in every Attempt.
        // A record in flight through a retire uses NEITHER (it is linked through
        // `head`), which is what makes the sharing safe rather than clever.
        Atomic<DeferredRelease*> next{nullptr};

        Core::rcu::RetireHead head{};

        DeferredRelease() = default;
        DeferredRelease(const DeferredRelease&)            = delete;
        DeferredRelease& operator=(const DeferredRelease&) = delete;
    };

    // ─── The per-CPU pool ──────────────────────────────────────────────────
    //
    // Multi-producer (any CPU's deleter pushes), SINGLE-CONSUMER (only the
    // owning CPU draws). That asymmetry is what makes a plain Treiber stack
    // ABA-safe here: ABA needs the head to return to a previously observed value
    // with different contents behind it, and reaching a previously popped node
    // again requires a second popper. There is none, by construction. Stated in
    // Ordering.h too, at kPoolPush/kPoolPop, so nobody adds one.
    struct alignas(arch::CACHE_LINE_SIZE) DeferredReleasePool {
        Atomic<DeferredRelease*> head{nullptr};

        // ITEM-084's evidence. The open question is whether the eager
        // worst-case-per-operation sizing is right or a smaller reserve backed
        // by the `barrier` fallback suffices, and it cannot be answered from
        // first principles — it needs the draw and shortfall distribution from a
        // realistic workload, which is what these are for.
        Atomic<uint64_t> draws{0};
        Atomic<uint64_t> shortfalls{0};

        // How many records are currently home. Not a correctness gate — see
        // kPoolAccounting — but the replenish path needs to distinguish "the
        // pump brought everything back" from "some records are still in flight",
        // and `empty()` cannot: a pool holding three records when the operation
        // needs five is not empty and is still short.
        Atomic<int64_t> depth{0};

        // §11's record-population conservation target: "each pool holds exactly
        // its creation population before the free". Written once at creation,
        // read at teardown.
        size_t population = 0;

        void push(DeferredRelease* r) {
            DeferredRelease* h = head.load(kQuiescedRead);
            for (;;) {
                r->next.store(h, kPrivateInit);
                if (head.compare_exchange(h, r, kPoolPush, kQuiescedRead)) break;
            }
            depth.fetch_add(1, kPoolAccounting);
        }

        [[nodiscard]] DeferredRelease* pop() {
            DeferredRelease* h = head.load(kPoolPop);
            for (;;) {
                if (h == nullptr) return nullptr;
                // The record is vmsmalloc-backed and may have been last written
                // by another CPU's deleter — this load of its `next` is the
                // first touch, and it precedes the CAS that takes ownership.
                DeferredRelease* n =
                    VMSubstrate::SafePtr<DeferredRelease>(h)->next.load(kQuiescedRead);
                if (head.compare_exchange(h, n, kPoolPop, kPoolPop)) {
                    depth.fetch_sub(1, kPoolAccounting);
                    return h;
                }
            }
        }

        [[nodiscard]] bool atFullPopulation() const {
            return depth.load(kPoolAccounting) >= static_cast<int64_t>(population);
        }
    };

    // ─── The deleter ───────────────────────────────────────────────────────
    //
    // Runs at grace-period end, wherever a pump runs. It releases the reference
    // the record carries and returns the record to its home pool — it does NOT
    // free the record (see the header comment: freeing re-creates the
    // allocating-`munmap` hazard one step removed).
    inline void deleteDeferredRelease(DeferredRelease* r) {
        // onPreTouch covered `head` and nothing else. Everything below is a
        // cross-CPU access to the record's body, so it goes through a SafePtr
        // rather than through a bare freshness call and a raw pointer.
        const VMSubstrate::SafePtr<DeferredRelease> rec(r);

        Mapping* const m               = rec->mapping;
        const uint64_t delta           = rec->delta;
        DeferredReleasePool* const home = rec->homePool;

        assert(m != nullptr, "radix DeferredRelease: retired record names no Mapping");
        assert(home != nullptr, "radix DeferredRelease: retired record has no home pool — "
                                "it can never be returned, which is a permanent shrink of "
                                "the fixed population");

        rec->mapping = nullptr;
        rec->delta   = 0;

        // Release BEFORE the push. After the push the record belongs to its
        // owner again and may be re-drawn and rewritten at any moment; the
        // locals above are what make the order a matter of clarity rather than
        // of correctness, and stating it as an order keeps it that way.
        // §7.1: "either deleter RMW-ing a `Mapping`'s count word". The record
        // was made fresh above; the RECORD IT NAMES is a separate allocation on
        // a separate page and owes its own call.
        releaseMappingRefs(VMSubstrate::SafePtr<Mapping>(m), delta);
        home->push(r);
    }

    // ─── ITEM-084's evidence: the draw-count histogram ─────────────────────
    //
    // DEC-068 sizes every (CPU x address space) pool at the PER-OPERATION
    // ceiling — the edge sum, 230 records under the kernel geometry, ~14.4 KiB
    // realised per CPU per address space. ITEM-084 asks whether that eager
    // sizing is right or whether a smaller reserve behind the abandon-and-
    // `barrier` replenish would do, and says the answer "cannot be answered from
    // first principles — it needs the draw and shortfall distribution from a
    // realistic workload".
    //
    // This is that distribution. What it records is the count an attempt HELD
    // when it committed, which is the quantity the pool must actually cover:
    // records are returned on abandonment, so an operation's peak demand is per
    // attempt, not per operation.
    //
    // Buckets are exponential above 4 because the interesting question is the
    // shape of the TAIL — §7.1 predicts "most operations draw one or two", and
    // whether the rest is a thin tail out to the ceiling or a wall at some
    // middle value is exactly what distinguishes ITEM-084's two answers. The top
    // bucket is open-ended and its own assertion: a draw above the derived
    // ceiling would mean the edge-sum bound is wrong, which is a different and
    // much worse finding than a badly chosen reserve.
    struct DrawHistogram {
        static constexpr unsigned kBuckets = 12;
        // 0, 1, 2, 3, 4, 5-8, 9-16, 17-32, 33-64, 65-128, 129-230, >230
        Atomic<uint64_t> counts[kBuckets] = {};
        Atomic<uint64_t> maxHeld{0};
        Atomic<uint64_t> operations{0};

        static unsigned bucketFor(uint64_t held) {
            if (held <= 4)   return static_cast<unsigned>(held);
            if (held <= 8)   return 5;
            if (held <= 16)  return 6;
            if (held <= 32)  return 7;
            if (held <= 64)  return 8;
            if (held <= 128) return 9;
            if (held <= 230) return 10;
            return 11;
        }

        void note(uint64_t held) {
            counts[bucketFor(held)].fetch_add(1, kCensusAccounting);
            operations.fetch_add(1, kCensusAccounting);
            uint64_t cur = maxHeld.load(kCensusAccounting);
            while (held > cur &&
                   !maxHeld.compare_exchange_weak(cur, held, kCensusAccounting,
                                                  kCensusAccounting)) {}
        }

        void reset() {
            for (unsigned i = 0; i < kBuckets; i++) counts[i].store(0, kCensusAccounting);
            maxHeld.store(0, kCensusAccounting);
            operations.store(0, kCensusAccounting);
        }

        [[nodiscard]] uint64_t bucket(unsigned i) const {
            return counts[i].load(kCensusAccounting);
        }
        [[nodiscard]] uint64_t max() const { return maxHeld.load(kCensusAccounting); }
        [[nodiscard]] uint64_t total() const { return operations.load(kCensusAccounting); }
    };

    // ─── DEC-077's evidence: the detachment-size distribution ──────────────
    //
    // `detachBudget` bounds the nodes ONE attempt may mark and claim; exceeding
    // it returns `NeedsDecomposition` and §6.5 splits the operation into units.
    // Claim.h brackets the shapes — a full C1 subtree is 33 nodes and never
    // decomposes at 64, a full C0 is 1,057 and always does — and says "Phase 5
    // measures it".
    //
    // What is recorded is the SIZE, not the decomposition rate, and that is the
    // better instrument: the rate at any budget follows from the distribution,
    // while a rate measured at one budget says nothing about another. The same
    // buckets as the draw histogram, so the two read alike.
    using DetachHistogram = DrawHistogram;
    // DEC-059's: objects destroyed by one pump, against `drainBatchBound`.
    using DrainHistogram = DrawHistogram;

#ifdef CROCOS_RADIX_DRAW_HISTOGRAM
    inline DetachHistogram gDetachHistogram;
    // Attempts that hit the budget and had to decompose.
    inline Atomic<uint64_t> gDecompositions{0};
    inline void noteDetachSize(uint64_t nodes) { if (nodes) gDetachHistogram.note(nodes); }
    inline void noteDecomposition() { gDecompositions.fetch_add(1, kCensusAccounting); }
    inline DrainHistogram gDrainHistogram;
    inline void noteDrainBatch(uint64_t destroyed) { gDrainHistogram.note(destroyed); }
    // DEC-095's two halves: probes consumed by a placement that succeeded in
    // stage 1, and chunks consumed by one that fell through to the stage-2 scan.
    inline DrawHistogram gProbeHistogram;
    inline DrawHistogram gScanChunkHistogram;
    inline Atomic<uint64_t> gProbeFallbacks{0};
    inline void noteProbeCount(uint64_t probes) { gProbeHistogram.note(probes); }
    inline void noteProbeFallback() { gProbeFallbacks.fetch_add(1, kCensusAccounting); }
    inline void noteScanChunks(uint64_t chunks) { gScanChunkHistogram.note(chunks); }
#else
    inline void noteDetachSize(uint64_t) {}
    inline void noteDecomposition() {}
    inline void noteDrainBatch(uint64_t) {}
    inline void noteProbeCount(uint64_t) {}
    inline void noteProbeFallback() {}
    inline void noteScanChunks(uint64_t) {}
#endif

#ifdef CROCOS_RADIX_DRAW_HISTOGRAM
    inline DrawHistogram gDrawHistogram;
    inline void noteDrawCount(uint64_t held) { gDrawHistogram.note(held); }
#else
    inline void noteDrawCount(uint64_t) {}
#endif

    // ─── The per-address-space pool array ──────────────────────────────────
    //
    // §7.1 puts the heads in the DEC-082 control block's pinned storage, which
    // is exactly why this is a plain embeddable aggregate with no allocation of
    // its own: the control block (a later Phase 3 item) will hold one by value.
    // Only the RECORDS are vmsmalloc allocations.
    //
    // The per-CPU ceiling is the **edge sum**, not the site bound — a different
    // governing quantity, and §7.1 is emphatic about it because the site bound
    // is the one already in the code. §6.1 budgets ZERO allocations for a
    // `munmap` covering fifteen of a C2 node's sixteen slots, and that operation
    // needs fifteen records.
    constexpr unsigned deferredReleaseBound(const GeometryDescriptor& g) {
        // The topmost node's valence — for a fully grown cluster that is level 1,
        // and the array is sized once for the worst case — plus two
        // partially-covered edge nodes per level below it, at valence-1 distinct
        // records each. Interior nodes contribute nothing: a fully covered child
        // dispatches to detach, where every displaced value rides a node deleter
        // and draws no record at all. (Which is the shape to remember: the
        // PARTIAL cover is the expensive one, and it is easy to invert.)
        unsigned n = valence(g, 1);
        for (unsigned l = 2; l <= g.levelCount; l++) n += 2u * (valence(g, l) - 1u);
        return n;
    }

    // §7.1's ≈230, derived rather than transcribed — DEC-093 requires that
    // retuning the split costs a descriptor edit and re-derived figures, and a
    // transcribed 230 would silently survive a retune that changed it.
    static_assert(deferredReleaseBound(kAmd64Geometry) == 230,
                  "the DEC-068 record ceiling is the edge sum: valence(level 1) plus "
                  "2*(valence-1) per level below. If this fires, the geometry was "
                  "retuned and §7.1's memory figures need re-deriving with it");

    // Deliberately NOT a template on the geometry, even though the per-CPU
    // ceiling is geometry-derived. The control block that will own this holds
    // trees of one geometry, but it is a runtime-sized pinned reservation in any
    // case, and a runtime `perCpu` lets one type serve every geometry the tests
    // instantiate. The geometry link is not lost: `create`'s caller passes
    // `deferredReleaseBound(G)`, and CoreTree::init asserts the pool it is handed
    // is at least that deep — so an undersized pool is caught at bind time
    // rather than as a mysterious shortfall much later.
    // Bytes of pool storage a given CPU count needs. Exported because the
    // storage is the CALLER's to provide — see the note on `create`.
    constexpr size_t deferredReleasePoolBytes(size_t cpus) {
        // cpus + 1: the extra slot is the per-ADDRESS-SPACE reserve (ITEM-084).
        return (cpus + 1) * sizeof(DeferredReleasePool);
    }

    // ─── ITEM-084's answer: a small per-CPU pool behind one reserve ────────
    //
    // DEC-068 sized every per-CPU pool at the per-OPERATION ceiling, which made
    // the per-address-space cost scale with `processorCount()` — ~115 KiB per
    // process on an 8-CPU desktop and ~3.6 MiB at the 256-CPU architectural
    // maximum, plus one `tryMake` per record at creation (1,840 allocations per
    // fork on 8 CPUs). D-054 measured the realistic draw at **2**.
    //
    // The split: a small per-CPU pool for the common case, and ONE reserve per
    // address space holding a full ceiling. The division of labour matters and
    // is worth stating plainly —
    //
    //   * **The reserve carries TERMINATION.** §7.1's replenish loop terminates
    //     only because a population covering one operation's worst case is
    //     reachable without allocating. That property now lives in the reserve
    //     rather than in every per-CPU pool, which is the whole saving.
    //   * **The per-CPU size is only about how OFTEN the reserve is touched.**
    //     Getting it wrong costs abandon-and-retry round trips, never progress.
    //
    // 32 is one node's worst partial cover at the widest valence in the kernel
    // geometry (a 32-slot node, 31 of them cleared), so the entire single-node
    // dense shape — the one D-054 could construct — stays off the reserve.
    inline constexpr unsigned kDefaultRecordsPerCpu = 32;

    // Shortfalls on one CPU before the address space is promoted to a larger
    // per-CPU population. Deliberately small: a shortfall already costs an
    // abandon and a retry, so there is little value in tolerating many before
    // reacting, and D-054's measurement says the common workload takes none at
    // all. A promoted address space never demotes — the workloads that produce
    // dense arrangements (`MAP_FIXED` packing, `fork`) are properties of a
    // process, not phases of one.
    inline constexpr unsigned kPromotionThreshold = 4;

    // Consecutive replenish rounds before a shortfall is treated as a defect
    // rather than as contention for the shared reserve (D-056). Generous on
    // purpose: the legitimate worst case is every other CPU of the address space
    // borrowing the reserve ahead of this one, each for a grace period, so a
    // bound tight enough to be interesting would fire on a busy machine. What it
    // still catches is the case worth catching — records that never come home.
    inline constexpr unsigned kShortfallRoundLimit = 64;

    struct DeferredReleasePools {
        // ─── The array is RUNTIME-SIZED, and the storage is the caller's ────
        //
        // It used to be `DeferredReleasePool pools[arch::MAX_PROCESSOR_COUNT]`
        // by value — 256 cache-line-aligned entries, **unconditionally**. That
        // is 16,448 bytes of a 16,704-byte control block: on an 8-CPU machine,
        // 98% of the per-address-space pinned reservation was padding for CPUs
        // that do not exist, and it set the static-buffer window's ceiling at
        // roughly 44,000 concurrent address spaces instead of half a million.
        //
        // A POINTER rather than an allocation of its own, because the pool heads
        // are the one thing that must not move to vmsmalloc: every CPU's
        // deleters push into every CPU's pool, so the heads sit on other CPUs'
        // hot paths, and DEC-082's round-4 amendment put them in pinned storage
        // precisely so they carry no `ensureTLBEntryFresh` obligation. Allocating
        // the array here would hand that obligation straight back.
        //
        // So the caller supplies storage from wherever it already has pinned
        // bytes — in the kernel, the tail of the control block's own
        // reservation, which keeps the "one block anchors everything" property
        // DEC-082 is built on.
        DeferredReleasePool* pools = nullptr;
        size_t cpuCount = 0;
        unsigned perCpu = 0;
        // ITEM-084's metadata, living in the control block by virtue of this
        // object living there. `shortfalls` drives promotion; `promotions` is
        // the evidence that it ever fired, which the measured workload says it
        // should not.
        unsigned reserveSize = 0;
        unsigned promotions  = 0;
        Atomic<uint64_t> shortfallEvents{0};

        // Allocate the fixed population, one `tryMake` per record (§7.1). Returns
        // false having freed everything it took — address-space creation is the
        // operation allowed to fail here, and DEC-101's unwind is why this must
        // leave nothing behind.
        //
        // `storage` must hold `deferredReleasePoolBytes(cpus)` bytes, be
        // cache-line aligned, and outlive these pools. It is NOT freed by
        // `destroy()`: the records are this object's, the array is not.
        [[nodiscard]] bool create(DeferredReleasePool* storage, size_t cpus,
                                  unsigned recordsPerCpu, unsigned reserveRecords) {
            assert(cpus <= arch::MAX_PROCESSOR_COUNT,
                   "radix DeferredReleasePools: CPU count exceeds the processor cap");
            assert(storage != nullptr, "radix DeferredReleasePools: no pool storage");
            assert(reinterpret_cast<uintptr_t>(storage) % arch::CACHE_LINE_SIZE == 0,
                   "radix DeferredReleasePools: pool storage must be cache-line aligned — "
                   "every CPU's deleters push into every CPU's head, and unaligned heads "
                   "reintroduce exactly the false sharing the alignas exists to prevent");
            pools    = storage;
            for (size_t c = 0; c <= cpus; c++) new (&pools[c]) DeferredReleasePool();
            cpuCount = cpus;
            perCpu   = recordsPerCpu;
            reserveSize = reserveRecords;
            // The reserve is filled FIRST, so a partial creation failure leaves
            // the termination-carrying pool short rather than absent — and
            // `destroy()` unwinds either way.
            for (size_t c = 0; c <= cpus; c++) {
                const unsigned want = (c == cpus) ? reserveRecords : recordsPerCpu;
                for (unsigned k = 0; k < want; k++) {
                    auto p = VMSubstrate::tryMake<DeferredRelease>();
                    if (!p) { destroy(); return false; }
                    auto* r = static_cast<DeferredRelease*>(p.raw());
                    r->mapping  = nullptr;
                    r->delta    = 0;
                    r->homePool = &pools[c];
                    pools[c].push(r);
                    pools[c].population++;
                }
            }
            return true;
        }

        [[nodiscard]] DeferredReleasePool& reserve() { return pools[cpuCount]; }
        [[nodiscard]] unsigned perCpuTarget() const { return perCpu; }
        [[nodiscard]] unsigned reserveDepth() const { return reserveSize; }
        [[nodiscard]] unsigned promotionCount() const { return promotions; }
        [[nodiscard]] uint64_t shortfallCount() const {
            return shortfallEvents.load(kPoolAccounting);
        }

        void noteShortfall(size_t cpu) {
            (void)cpu;
            shortfallEvents.fetch_add(1, kPoolAccounting);
        }

        // Caller holds the domain-management lock (promotion mutates `perCpu`
        // and the pools). The threshold is counted per ADDRESS SPACE rather than
        // per CPU: the dense shapes this reacts to are properties of a process's
        // allocation pattern, and one CPU seeing them means the others will.
        [[nodiscard]] bool shouldPromote() const {
            return shortfallEvents.load(kPoolAccounting) >=
                   uint64_t{kPromotionThreshold} * (promotions + 1);
        }

        // ─── The bulk refill (ITEM-084) ────────────────────────────────────
        //
        // **Bulk, and that is the correctness point rather than an efficiency
        // one.** `deferMappingRelease` draws ONE record at a time as it
        // discovers distinct mappings and never knows how many it will need, so
        // a per-record fallback to a shared reserve lets two CPUs each hold a
        // fragment of it with neither able to finish — both abandon, both retry,
        // and they can ping-pong. Moving a whole operation's worth at once means
        // a CPU either has enough for any shape or abandons immediately, and the
        // reserve is never held in fragments.
        //
        // **Called from `replenishRecords`, never from the draw**, because the
        // draw runs inside an attempt's read section and this takes the
        // domain-management lock — blocking while holding claims, which §3.2
        // forbids. The existing abandon-and-retry path is already outside every
        // section, which is exactly where this belongs.
        //
        // Loaned records keep `homePool` pointing at the RESERVE, so they find
        // their own way back when their deleters run. There is no return path to
        // write.
        size_t refillFromReserve(size_t cpu, unsigned want) {
            assert(cpu < cpuCount, "radix DeferredReleasePools: refill for no such CPU");
            size_t moved = 0;
            for (unsigned k = 0; k < want; k++) {
                DeferredRelease* r = reserve().pop();
                if (r == nullptr) break;          // reserve empty: caller falls through
                pools[cpu].push(r);
                moved++;
            }
            return moved;
        }

        // ─── The return path, without which the reserve is a livelock ──────
        //
        // A refill lends this CPU up to a whole attempt's worth. **Nothing else
        // ever makes it give them back**, and that is a livelock rather than an
        // inefficiency: a CPU that borrowed 230 records for an operation needing
        // 3 leaves the reserve empty, and the next CPU to need a wide attempt
        // finds nothing there, barriers, recovers only its OWN retirees, and
        // never proceeds. Found by the promotion test, which stopped seeing
        // shortfalls after the first one because the borrower never released.
        //
        // So: at the end of every operation, everything above this CPU's own
        // target goes home. Records are routed by `homePool`, so loans land back
        // in the reserve and natives stay put — the population accounting stays
        // exact rather than drifting between pools.
        void returnSurplus(size_t cpu) {
            assert(cpu < cpuCount, "radix DeferredReleasePools: surplus for no such CPU");
            DeferredReleasePool& mine = pools[cpu];
            if (mine.depth.load(kPoolAccounting) <= static_cast<int64_t>(perCpu)) return;

            // Drain and re-file. O(depth), and reached only after a refill,
            // which D-054 measures at zero occurrences in a realistic workload.
            DeferredRelease* natives = nullptr;
            while (DeferredRelease* r = mine.pop()) {
                if (r->homePool == &mine) {
                    r->next.store(natives, kPrivateInit);
                    natives = r;
                } else {
                    r->homePool->push(r);
                }
            }
            while (natives != nullptr) {
                DeferredRelease* const next = natives->next.load(kPrivateInit);
                mine.push(natives);
                natives = next;
            }
        }

        // Promotion (ITEM-084). Grows the per-CPU population for an address
        // space that keeps taking shortfalls, capped at the ceiling — above
        // which the reserve is the answer, not a bigger per-CPU pool.
        //
        // **Failure is harmless by construction**: termination rests on the
        // reserve, so a promotion that cannot allocate leaves a correct system
        // that merely visits the reserve more often. That is why this is
        // `tryMake` with no unwind and no status.
        void promote() {
            if (perCpu >= reserveSize) return;                  // already at the cap
            const unsigned target = (perCpu * 2 < reserveSize) ? perCpu * 2 : reserveSize;
            for (size_t c = 0; c < cpuCount; c++) {
                for (unsigned k = perCpu; k < target; k++) {
                    auto p = VMSubstrate::tryMake<DeferredRelease>();
                    if (!p) return;                             // harmless; see above
                    auto* r = static_cast<DeferredRelease*>(p.raw());
                    r->mapping  = nullptr;
                    r->delta    = 0;
                    r->homePool = &pools[c];
                    pools[c].push(r);
                    pools[c].population++;
                }
            }
            perCpu = target;
            promotions++;
        }

        // §7.4: the pools are freed at teardown, record by record, AFTER
        // `drainAllQuiescent` has returned every record home. The population
        // check is §11's conservation target, and it is the only detector for a
        // record that was drawn and neither retired nor returned — which
        // presents as nothing at all until the pool runs dry under load much
        // later.
        void destroy() {
            // Idempotent. `destroy()` nulls the array below, and it is reachable
            // twice for real reasons: DEC-101's creation unwind calls it on a
            // partial failure, and a caller may release the pools before the
            // object's own teardown. The second call dereferencing a null array
            // is a SEGV, which is how this was found.
            if (pools == nullptr) return;
            // §11's conservation check is now a TOTAL rather than per-pool, and
            // the weakening is forced by the reserve: a refill moves records
            // between pools, and an unused loan is still sitting in the CPU pool
            // it was lent to at teardown. The total still catches the failure
            // this exists for — a record drawn and never returned — it just no
            // longer says which pool lost it.
            size_t recoveredTotal = 0, populationTotal = 0;
            for (size_t c = 0; c <= cpuCount; c++) {
                size_t recovered = 0;
                while (DeferredRelease* r = pools[c].pop()) {
                    VMSubstrate::destroy(VMSubstrate::SafePtr<DeferredRelease>(r));
                    recovered++;
                }
                recoveredTotal  += recovered;
                populationTotal += pools[c].population;
                pools[c].population = 0;
                pools[c].draws.store(0, kPoolAccounting);
                pools[c].shortfalls.store(0, kPoolAccounting);
                pools[c].depth.store(0, kPoolAccounting);
            }
            assert(recoveredTotal == populationTotal,
                   "radix DeferredReleasePools: the pools did not hold their creation "
                   "population at teardown — a record was drawn and never returned, or "
                   "drainAllQuiescent ran before every retire completed (§11)");
            cpuCount = 0;
            perCpu   = 0;
            reserveSize = 0;
            // The array itself is the caller's storage and is deliberately NOT
            // released here — it is a slice of a pinned reservation that
            // outlives this object and is reused by the next tenant.
            pools    = nullptr;
        }

        // Where the array actually is. Exists so a test can assert the pools
        // point at the block's own tail rather than at a previous tenant's —
        // a stale base would be a live record list belonging to a dead process.
        [[nodiscard]] const DeferredReleasePool* poolsBase() const { return pools; }

        [[nodiscard]] DeferredReleasePool& forCpu(arch::ProcessorID i) {
            assert(static_cast<size_t>(i) < cpuCount,
                   "radix DeferredReleasePools: CPU index outside the created range");
            return pools[static_cast<size_t>(i)];
        }
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_DEFERRED_RELEASE_H
