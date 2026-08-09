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
        return cpus * sizeof(DeferredReleasePool);
    }

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

        // Allocate the fixed population, one `tryMake` per record (§7.1). Returns
        // false having freed everything it took — address-space creation is the
        // operation allowed to fail here, and DEC-101's unwind is why this must
        // leave nothing behind.
        //
        // `storage` must hold `deferredReleasePoolBytes(cpus)` bytes, be
        // cache-line aligned, and outlive these pools. It is NOT freed by
        // `destroy()`: the records are this object's, the array is not.
        [[nodiscard]] bool create(DeferredReleasePool* storage, size_t cpus,
                                  unsigned recordsPerCpu) {
            assert(cpus <= arch::MAX_PROCESSOR_COUNT,
                   "radix DeferredReleasePools: CPU count exceeds the processor cap");
            assert(storage != nullptr, "radix DeferredReleasePools: no pool storage");
            assert(reinterpret_cast<uintptr_t>(storage) % arch::CACHE_LINE_SIZE == 0,
                   "radix DeferredReleasePools: pool storage must be cache-line aligned — "
                   "every CPU's deleters push into every CPU's head, and unaligned heads "
                   "reintroduce exactly the false sharing the alignas exists to prevent");
            pools    = storage;
            for (size_t c = 0; c < cpus; c++) new (&pools[c]) DeferredReleasePool();
            cpuCount = cpus;
            perCpu   = recordsPerCpu;
            for (size_t c = 0; c < cpus; c++) {
                for (unsigned k = 0; k < recordsPerCpu; k++) {
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

        // §7.4: the pools are freed at teardown, record by record, AFTER
        // `drainAllQuiescent` has returned every record home. The population
        // check is §11's conservation target, and it is the only detector for a
        // record that was drawn and neither retired nor returned — which
        // presents as nothing at all until the pool runs dry under load much
        // later.
        void destroy() {
            for (size_t c = 0; c < cpuCount; c++) {
                size_t recovered = 0;
                while (DeferredRelease* r = pools[c].pop()) {
                    VMSubstrate::destroy(VMSubstrate::SafePtr<DeferredRelease>(r));
                    recovered++;
                }
                assert(recovered == pools[c].population,
                       "radix DeferredReleasePools: a pool did not hold its creation "
                       "population at teardown — a record was drawn and never returned, "
                       "or drainAllQuiescent ran before every retire completed (§11)");
                pools[c].population = 0;
                pools[c].draws.store(0, kPoolAccounting);
                pools[c].shortfalls.store(0, kPoolAccounting);
                pools[c].depth.store(0, kPoolAccounting);
            }
            cpuCount = 0;
            perCpu   = 0;
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
