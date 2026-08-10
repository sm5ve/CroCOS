//
// radix-tree Phase 5 — the in-kernel stress.
//
// The tree has 150 green userspace tests on two sanitizers and has **never
// executed a single instruction on the target**. Everything below §12 in the
// spec is written against a mock `VMSubstrate` whose `ensureTLBEntryFresh` is a
// no-op, whose codec base is an `mmap` address, and whose "physical exhaustion"
// is a scripted null. This is the first build where the tree meets real
// vmsmalloc, real RCU domains, real page tables and the real compressed codec.
//
// The precedent is exact and deliberate: vmsmalloc's DEC-047 stale-TLB bug was
// found by an in-kernel stress and was structurally invisible to its userspace
// harness, and RCU Phase 4 repeated the shape for the same reason. So the
// interesting thing here is not "does the tree work" — the model check answers
// that better than any stress can — but the four things the harness cannot see:
//
//   1. **The kernel codec's base and window.** §10: "the harness is structurally
//      blind to the kernel codec's compression arithmetic." A base derived from
//      the wrong page-table index encodes and decodes perfectly and names memory
//      the tree does not own. `verifyKernelCodecBase()` is the check; this is
//      where it first runs.
//   2. **`ensureTLBEntryFresh` on the tree's own paths.** Nodes come from
//      vmsmalloc, whose pages are subject to `reclaimSlabPage`, and every read
//      of allocator-returned memory has to be fresh on THIS CPU. §7.1's
//      deleter-body and record-first-draw sites are the class the mock cannot
//      catch.
//   3. **Real allocation failure.** The harness scripts nulls; here `tryMake`
//      fails when physical memory is genuinely gone, at whatever site happens to
//      be running. §10's inverted hazard is that a site written against
//      never-null `make<T>` re-imports a userspace-triggerable panic.
//   4. **The residue gate**, which §11 states against the DEC-096 BOUND rather
//      than zero and which nothing in userspace measures in kernel terms.
//
// ─── The shape ────────────────────────────────────────────────────────────
//
// Cycles, not a flat loop. Every CPU churns its own cluster for a fixed number
// of operations; all CPUs meet at a barrier; CPU 0 then tears the address space
// down, measures the residue twice — once with every CPU's descent-cache entries
// still held, once after they are all evicted — and builds a fresh one.
//
// The two measurements are the point. The first is DEC-096's claim that residue
// is BOUNDED (`processorCount() x cacheEntries` nodes, plus D-030's stranded
// roots); the second is its claim that the bound is TEMPORARY — "released as
// each CPU's cache turns over". A design that leaked instead of pinning passes
// the first and fails the second.
//
// ─── What is deliberately not here ────────────────────────────────────────
//
// No correctness oracle. The userspace harness has the shadow interval map, the
// exhaustive model check and the DEC-052 poison oracle, and all three are
// structurally unavailable on the target. What this run can say is: the tree
// executes, the asserts do not fire, the memory comes back. That is the coverage
// this phase is for, and claiming more from it would be claiming the harness's
// results twice.
//

#include <kernel.h>
#include <arch.h>
#include <kassert.h>
#include <kexit.h>
#include <CpuLocal.h>
#include <Random.h>
#include <core/atomic.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>
#include <rcu/RCU.h>
#include <timing/timing.h>

#include <mem/radix/KernelInstance.h>

namespace VMS = kernel::mm::VMSubstrate;

namespace kernel {

namespace radix_stress {

    namespace rdx = kernel::mm::radix;

    constexpr size_t   kMaxCpus       = 16;    // matches the run / qmon -smp counts
    constexpr uint64_t kLivenessMask  = 0x3FFull;

    // Operations per CPU per cycle. Radix operations are one to three orders of
    // magnitude heavier than RCU Phase 4's read/retire pair — a placement is a
    // probe loop over a claimed two-pass attempt — so the per-cycle count is
    // small and the cycle count is what accumulates coverage.
    // **Also the knob that makes the address-space LIFECYCLE the thing under test
    // rather than the churn**, which is why it is a build setting
    // (`-DCROCOS_RADIX_STRESS_OPS=24`) and not a constant to edit. At the
    // default a debug build reaches 4 cycles inside the 20 s shutdown window; at
    // 24 it reaches 1025, which is what turned D-041's Release-only failures
    // into a debug reproduction with every assert live. Lower it before
    // investigating anything create/teardown shaped.
#ifndef CROCOS_RADIX_STRESS_OPS
#define CROCOS_RADIX_STRESS_OPS 512
#endif
    constexpr unsigned kOpsPerCycle   = CROCOS_RADIX_STRESS_OPS;

    // Live mappings a CPU keeps at once. Small enough that the unmap and
    // MAP_FIXED rows fire constantly rather than being crowded out by
    // placements, which is what makes nodes actually get reclaimed — and node
    // reclamation is the precondition for reaching the DEC-047 hazard class at
    // all. A workload that only ever grows the tree exercises none of it.
    constexpr unsigned kLiveSlots     = 24;

    // Placement sizes, in 64 KiB placement granules (DEC-013). Spread so the
    // tree subdivides to several depths rather than settling at one level:
    // leaves live at any level, and a workload that produces only one leaf level
    // exercises one dispatch row.
    constexpr uint64_t kGranule       = rdx::kPlacementGranularity;

    // ── The address space under test ───────────────────────────────────────
    //
    // Storage, not objects. Per [[project_crocos_static_init_rule]] a global
    // constructor here would run during cpp_init, before VMSubstrate exists —
    // which is exactly how PITEventSource panicked — so everything with a
    // non-trivial constructor is placement-newed at stress start instead. The
    // descent cache in particular is ~64 KiB of per-CPU rows whose default
    // constructor is a loop.
    alignas(rdx::KernelBlocks) unsigned char gStoreStorage[sizeof(rdx::KernelBlocks)];
    alignas(rdx::KernelCache)  unsigned char gCacheStorage[sizeof(rdx::KernelCache)];
    alignas(rdx::PerCpuAssignment)
        unsigned char gAssignStorage[sizeof(rdx::PerCpuAssignment)];

    rdx::KernelBlocks*      gStore    = nullptr;
    rdx::KernelCache*       gCache    = nullptr;
    rdx::PerCpuAssignment*  gAssign   = nullptr;

    // Published with a release store by CPU 0; every AP acquire-loads it and
    // spins until it is non-null. The block's contents are ordered by this
    // store, which is the same edge `createAddressSpace`'s own publish step
    // relies on (DEC-101 step 5).
    Atomic<rdx::KernelBlock*> gSpace{nullptr};

    // ── Counters ───────────────────────────────────────────────────────────
    //
    // All RELAXED diagnostics. The ones that are load-bearing rather than
    // decorative are marked: a zero in any of them means a whole row of the
    // workload never ran, which is the failure mode a healthy-looking stress
    // has.
    Atomic<uint64_t> gPlacements{0};      // load-bearing
    Atomic<uint64_t> gNoSpace{0};
    Atomic<uint64_t> gUnmaps{0};          // load-bearing: no unmaps, no reclamation
    Atomic<uint64_t> gFixed{0};           // load-bearing: no MAP_FIXED, no detachment
    Atomic<uint64_t> gLookups{0};
    Atomic<uint64_t> gBulkUnmaps{0};
    Atomic<uint64_t> gDenseRegions{0};
    Atomic<uint64_t> gOom{0};
    Atomic<uint64_t> gGrowths{0};
    Atomic<uint64_t> gClusters{0};        // D-030's stranded-root allowance is 2x this
    Atomic<uint64_t> gCycles{0};
    Atomic<uint64_t> gResiduePinned{0};   // last cycle's, with caches held
    Atomic<uint64_t> gResidueEvicted{0};  // ...and after every CPU evicted


    // ── D-052/D-042: the descent's instruction cost ────────────────────────
    //
    // The userspace harness measured the freshness call COUNT exactly (1.00 per
    // level on the read path, D-052) and can say nothing about what a call
    // costs, having no page tables. This is the other half, and it is RCU
    // P4-ITEM-002's machinery applied one subsystem over — same reasoning,
    // deliberately the same shape, so the two numbers are comparable.
    //
    // Under `-icount shift=0` QEMU derives the guest TSC from a virtual clock
    // that advances per instruction retired, so an rdtsc delta is proportional
    // to instructions. The constant of proportionality does not matter: the
    // headline is the RATIO of mode 1 to mode 2, measured in the same units.
    //
    // Two modes, never both, because they would nest: mode 1 brackets one
    // freshness call, mode 2 brackets a whole `lookup`. Mode 1's probes live
    // inside a mode-2 bracket would inflate the denominator by their own cost.
#ifdef CROCOS_RADIX_INSN_PROBE
    inline uint64_t insnCounter() noexcept {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // MINIMA, not means, for the reason P4-ITEM-002 records: interrupts are
    // enabled in the stress loop, so a timer landing inside a bracket inflates
    // that sample by the whole handler, and the contamination does not average
    // out — it dilutes with sample count. The minimum over hundreds of thousands
    // of samples is the uncontaminated path. Totals and counts are kept so a
    // mean is still readable and the sample size is visible.
    Atomic<uint64_t> gProbeFreshMin{0};
    Atomic<uint64_t> gProbeFreshTicks{0};
    Atomic<uint64_t> gProbeFreshCount{0};
    Atomic<uint64_t> gProbeStaleMin{0};
    Atomic<uint64_t> gProbeStaleCount{0};
    Atomic<uint64_t> gProbeLookupMin{0};
    Atomic<uint64_t> gProbeLookupTicks{0};
    Atomic<uint64_t> gProbeLookupCount{0};
    // An empty back-to-back rdtsc pair, so every figure can be stated net of the
    // probe itself rather than including it.
    Atomic<uint64_t> gProbeBaseMin{0};
    Atomic<uint64_t> gProbeBaseCount{0};

    void recordMin(Atomic<uint64_t>& slot, uint64_t sample) noexcept {
        uint64_t cur = slot.load(RELAXED);
        while ((cur == 0 || sample < cur) &&
               !slot.compare_exchange_weak(cur, sample, RELAXED, RELAXED)) {}
    }

    void calibrateProbe() noexcept {
        const uint64_t c0 = insnCounter();
        const uint64_t c1 = insnCounter();
        recordMin(gProbeBaseMin, c1 - c0);
        gProbeBaseCount.fetch_add(1, RELAXED);
    }
#endif

    // ── Heartbeat and watchdog ─────────────────────────────────────────────
    //
    // Same mechanism as RCU Phase 4's, and needed for the same reason: the
    // shutdown timer masks hangs, so a livelocked claim acquisition, a stalled
    // grace period and a healthy run would all end with `Goodbye :)` and exit 0.
    //
    // The barrier below makes this MORE necessary, not less: a CPU that wedges
    // inside an attempt takes every other CPU down with it at the barrier, and
    // without a watchdog the whole machine simply stops making noise.
    struct alignas(arch::CACHE_LINE_SIZE) Heartbeat {
        Atomic<uint64_t> beat{0};
    };
    Heartbeat gHeartbeat[kMaxCpus];

    uint64_t gPrevBeat[kMaxCpus];
    uint32_t gStallSamples[kMaxCpus];

    constexpr uint64_t kWatchdogFirstMs  = 4000;
    constexpr uint64_t kWatchdogPeriodMs = 2000;
    constexpr uint32_t kStallSamples     = 4;

    void watchdogTick();

    // Runs inside the watchdog's TIMER CALLBACK, i.e. in interrupt context, so
    // every line here goes to the unlocked stream: `klog` holds a global
    // spinlock for its whole statement and would self-deadlock against a
    // `klog` this CPU was interrupted in the middle of. A watchdog that hangs
    // instead of reporting is worse than no watchdog — the whole point of this
    // function is to be the last thing that still works.
    void reportHangAndExit(size_t stalled, size_t cpus) {
        emergencyLog() << "\nradixStress: WATCHDOG — cpu=" << static_cast<uint64_t>(stalled)
               << " made no progress across " << static_cast<uint64_t>(kStallSamples)
               << " samples (" << (kStallSamples * kWatchdogPeriodMs) << " ms)\n";
        for (size_t i = 0; i < cpus && i < kMaxCpus; ++i) {
            const uint64_t b = gHeartbeat[i].beat.load(RELAXED);
            emergencyLog() << "  cpu=" << static_cast<uint64_t>(i) << " beat=";
            if (b == 0) emergencyLog() << "(never started)\n"; else emergencyLog() << b << "\n";
        }
        exitToHost(ExitStatus::Hang);
    }

    void watchdogTick() {
        const size_t cpus = arch::processorCount();
        for (size_t i = 0; i < cpus && i < kMaxCpus; ++i) {
            const uint64_t cur = gHeartbeat[i].beat.load(RELAXED);
            if (cur != gPrevBeat[i]) {
                gPrevBeat[i] = cur;
                gStallSamples[i] = 0;
                continue;
            }
            if (++gStallSamples[i] >= kStallSamples) reportHangAndExit(i, cpus);
        }
        timing::enqueueEvent([] { watchdogTick(); }, kWatchdogPeriodMs);
    }

    // ── The cycle barrier ──────────────────────────────────────────────────
    //
    // Sense-reversing, and it beats the heartbeat while it spins: a CPU waiting
    // at the barrier IS making progress, and a watchdog that could not tell the
    // difference would fire on every cycle.
    Atomic<uint64_t> gBarrierCount{0};
    Atomic<uint64_t> gBarrierSense{0};

    void barrierWait(size_t cpus, arch::ProcessorID me, uint64_t& localSense, uint64_t& beat) {
        localSense ^= 1;
        if (gBarrierCount.fetch_add(1, ACQ_REL) == cpus - 1) {
            gBarrierCount.store(0, RELAXED);
            gBarrierSense.store(localSense, RELEASE);
        } else {
            while (gBarrierSense.load(ACQUIRE) != localSense) {
                gHeartbeat[me].beat.store(++beat, RELAXED);
                tight_spin();
            }
        }
    }

    // ── Mapping records ────────────────────────────────────────────────────
    //
    // Failable, like everything else the tree allocates: §10's inverted hazard
    // is that a site written against never-null `make<T>` re-imports the
    // userspace-triggerable panic through the back door. The stress reaches this
    // path from the same direction `mmap` will.
    rdx::Mapping* tryMakeMapping(uint64_t baseVA) {
        auto p = VMS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                            rdx::Protection::Read | rdx::Protection::Write,
                                            rdx::Protection::Read | rdx::Protection::Write);
        if (!p) { gOom.fetch_add(1, RELAXED); return nullptr; }
        rdx::noteMappingCreated();
        return static_cast<rdx::Mapping*>(p.raw());
    }

    // ── Per-CPU live set ───────────────────────────────────────────────────
    //
    // What this CPU has placed and can therefore unmap or overwrite. Per-CPU and
    // plain: only the owning CPU touches its row, which is the same
    // pinned-writer discipline the DEC-091 assignment cell and the descent
    // cache's rows already use.
    struct alignas(arch::CACHE_LINE_SIZE) LiveSet {
        uint64_t va[kLiveSlots];
        uint64_t span[kLiveSlots];
        unsigned count = 0;
    };
    LiveSet gLive[kMaxCpus];

    // ── One cycle's worth of churn on one CPU ──────────────────────────────
    void churn(rdx::KernelBlock& block, arch::ProcessorID me, uint64_t& rng, uint64_t& beat) {
        auto next = [&rng] {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            return rng;
        };
        LiveSet& live = gLive[static_cast<size_t>(me)];

        for (unsigned op = 0; op < kOpsPerCycle; op++) {
            const uint64_t r = next();

            // DEC-091's seam: which cluster this CPU works in is policy, and the
            // cell holds a BUCKET INDEX, never a decoded pointer.
            const size_t bucket = gAssign->bucketFor(me, r);
            const uint64_t clusterLo =
                static_cast<uint64_t>(bucket) << rdx::slotSpanBits(rdx::kAmd64Geometry, 0);

            // Bring the cluster into existence on demand. The default root level
            // covers 1 GiB, which every placement below fits inside, so this
            // creates and never grows on the common path — growth gets its own
            // row so it is exercised deliberately rather than incidentally.
            if (!block.clusters.bucketIsOccupied(bucket)) {
                const auto st = block.clusters.createCluster(
                    clusterLo, rdx::kAmd64Geometry.defaultRootLevel);
                if (st == rdx::ClusterStatus::OutOfMemory) {
                    gOom.fetch_add(1, RELAXED);
                    continue;
                }
                if (st == rdx::ClusterStatus::Ok) gClusters.fetch_add(1, RELAXED);
            }
            rdx::KernelTree tree = block.clusters.treeFor(bucket);

            const unsigned row = static_cast<unsigned>((r >> 17) % 16);

            if (row < 6 || live.count == 0) {
                // ── Probed placement (§5.6, DEC-095) ───────────────────────
                if (live.count == kLiveSlots) {
                    // Full: drop the oldest to make room, so the live set is a
                    // steady state rather than a ratchet.
                    const uint64_t va = live.va[0];
                    const uint64_t sp = live.span[0];
                    rdx::KernelTree t2 = block.clusters.treeFor(
                        rdx::bucketIndexFor<rdx::kAmd64Geometry>(va));
                    (void)t2.apply(va, va + sp - 1, nullptr);
                    gUnmaps.fetch_add(1, RELAXED);
                    for (unsigned i = 1; i < live.count; i++) {
                        live.va[i - 1] = live.va[i];
                        live.span[i - 1] = live.span[i];
                    }
                    live.count--;
                }
                const uint64_t granules = 1 + ((r >> 33) % 8);
                const uint64_t bytes = granules * kGranule;
                rdx::Mapping* m = tryMakeMapping(0);
                if (m == nullptr) continue;
                const auto res = rdx::placeInCluster(tree, bytes, m,
                                                     kernel::random::Entropy{});
                if (res.status == rdx::PlaceStatus::Ok) {
                    // `baseVA` is deliberately left at 0. A probed placement
                    // only learns its VA after `apply` has already published the
                    // record, and writing the field then would be a plain store
                    // into an object other CPUs are already reading — the VMM
                    // will mint records at a known address, so this is a
                    // property of the stress's probe loop and not of the API.
                    live.va[live.count] = res.va;
                    live.span[live.count] = bytes;
                    live.count++;
                    gPlacements.fetch_add(1, RELAXED);
                } else {
                    // Not placed, so nothing ever named it: a direct destroy of
                    // a never-published allocation (§7.3's shallow discard).
                    rdx::noteMappingDestroyed();
                    VMS::destroy(VMS::SafePtr<rdx::Mapping>(m));
                    if (res.status == rdx::PlaceStatus::NoSpaceInCluster) {
                        gNoSpace.fetch_add(1, RELAXED);
                        gAssign->noteOutOfSpace(me, bucket);
                    } else {
                        gOom.fetch_add(1, RELAXED);
                    }
                }
            } else if (row < 11) {
                // ── munmap ─────────────────────────────────────────────────
                const unsigned k = static_cast<unsigned>((r >> 41) % live.count);
                const uint64_t va = live.va[k];
                const uint64_t sp = live.span[k];
                rdx::KernelTree t2 = block.clusters.treeFor(
                    rdx::bucketIndexFor<rdx::kAmd64Geometry>(va));
                // A partial unmap half the time: an in-place range shrink that
                // retires nothing is a different dispatch row from a full clear,
                // and it is the shape DEC-051's validity token exists for.
                if ((r >> 50) & 1 && sp > kGranule) {
                    (void)t2.apply(va, va + kGranule - 1, nullptr);
                    live.va[k] = va + kGranule;
                    live.span[k] = sp - kGranule;
                } else {
                    (void)t2.apply(va, va + sp - 1, nullptr);
                    for (unsigned i = k + 1; i < live.count; i++) {
                        live.va[i - 1] = live.va[i];
                        live.span[i - 1] = live.span[i];
                    }
                    live.count--;
                }
                gUnmaps.fetch_add(1, RELAXED);
            } else if (row < 13) {
                // ── MAP_FIXED over a live range (§6.5's detachment) ────────
                const unsigned k = static_cast<unsigned>((r >> 41) % live.count);
                const uint64_t va = live.va[k];
                const uint64_t sp = live.span[k];
                rdx::Mapping* m = tryMakeMapping(va);
                if (m == nullptr) continue;
                rdx::KernelTree t2 = block.clusters.treeFor(
                    rdx::bucketIndexFor<rdx::kAmd64Geometry>(va));
                const auto st = t2.apply(va, va + sp - 1, m, rdx::ApplyMode::Overwrite);
                if (st == rdx::ApplyStatus::Ok) {
                    gFixed.fetch_add(1, RELAXED);
                } else {
                    // NOTE: `apply` returning OutOfMemory after a §6.5
                    // decomposition does NOT mean nothing was published —
                    // earlier units have committed. Destroying the record here
                    // would be a destroy of a still-referenced object. The
                    // decomposed shape is not reachable from this row (a
                    // MAP_FIXED over one placement's own range is one unit), so
                    // the discard is sound; a caller that could decompose would
                    // have to leak instead. Recorded in the deviations log.
                    rdx::noteMappingDestroyed();
                    VMS::destroy(VMS::SafePtr<rdx::Mapping>(m));
                    gOom.fetch_add(1, RELAXED);
                }
            } else if (row == 13) {
                // ── A dense region, built then torn down whole ─────────────
                //
                // **The one shape no other row can make, and the reason two
                // calibration items had no data.** Every other row places
                // through DEC-032's probe, which inserts random guard gaps
                // precisely so mappings are NOT adjacent — so nodes hold one
                // record apiece, the tree barely subdivides, and a slot whose
                // child is fully covered by an operation's range essentially
                // never occurs. `detachBudget` was therefore measured at zero
                // detachments in 1.9M attempts, which says nothing about the
                // budget.
                //
                // This builds the opposite on purpose: distinct records in
                // ADJACENT granules, which forces subdivision, and then one
                // operation over the whole run, which fully covers the child
                // and dispatches to DetachChild. It is a realistic shape as well
                // as a useful one — a linker mapping a library's segments
                // individually and unmapping it as a unit does exactly this.
                // **PAGE-granular, and that is the whole trick.** A record
                // covering a slot's entire span is stored as a LEAF in that
                // slot — so a run of adjacent GRANULE-sized records fills a
                // level-5 node's leaf slots and subdivides nothing, which is why
                // the first version of this row produced 102,408 dense regions
                // and still zero detachments. `kPlacementGranularity` is 64 KiB,
                // which is exactly `nodeSpan(level 6)`. Records SMALLER than the
                // slot are what force a child node to exist.
                //
                // One granule densified with pages = one level-6 node under a
                // level-5 slot. Unmapping whole granules then fully covers those
                // children and dispatches to DetachChild. Sixteen of them is a
                // whole level-5 node, which detaches as 1 + 16 = 17.
                const bool     wide  = ((r >> 33) & 7) == 0;
                const unsigned gran  = wide ? 16u : 1u + static_cast<unsigned>((r >> 37) % 4);
                const uint64_t base  = clusterLo + ((r >> 45) % 256) * kGranule * 16;
                const uint64_t pages = kGranule / arch::smallPageSize;
                unsigned built = 0;
                for (unsigned g = 0; g < gran; g++) {
                    for (uint64_t i = 0; i < pages; i++) {
                        const uint64_t va = base + g * kGranule + i * arch::smallPageSize;
                        rdx::Mapping* m = tryMakeMapping(va);
                        if (m == nullptr) { g = gran; break; }
                        const auto st = tree.apply(va, va + arch::smallPageSize - 1, m,
                                                   rdx::ApplyMode::Overwrite);
                        if (st != rdx::ApplyStatus::Ok) {
                            // Never published: §7.3's shallow discard.
                            rdx::noteMappingDestroyed();
                            VMS::destroy(VMS::SafePtr<rdx::Mapping>(m));
                            g = gran;
                            break;
                        }
                    }
                    if (g < gran) built++;
                }
                if (built != 0) {
                    // One operation over the whole run: a FULL cover of every
                    // child it built, so every release rides a node deleter and
                    // this draws no records at all — the cheap row of §7.1 and
                    // the expensive one for DEC-077, which is the point.
                    (void)tree.apply(base, base + built * kGranule - 1, nullptr);
                    gDenseRegions.fetch_add(1, RELAXED);
                }
            } else if (row == 14) {
                // ── Bulk unmap: a range covering SEVERAL distinct records ──
                //
                // Every other unmap row clears exactly the span it placed — a
                // full cover of ONE record — which is why the whole workload
                // never drew more than two `DeferredRelease` records and
                // ITEM-084 could not be answered from it. §7.1's expensive shape
                // is a PARTIAL cover displacing many distinct records at once,
                // and this is the row that produces it.
                //
                // It is also the more realistic `munmap`: a process tearing down
                // a region unmaps a RANGE, not one mapping at a time, and
                // whatever mappings lie in it — this CPU's or another's — go
                // with it. Live-set entries for records destroyed by another
                // CPU's bulk unmap are left to age out: the later unmap of an
                // already-empty range is a no-op that draws nothing, so a stale
                // entry costs a wasted operation and nothing else.
                const uint64_t span = (1 + ((r >> 37) % 32)) * kGranule;
                const uint64_t lo   = clusterLo + ((r >> 45) % 4096) * kGranule;
                (void)tree.apply(lo, lo + span - 1, nullptr);
                gBulkUnmaps.fetch_add(1, RELAXED);
            } else {
                // ── Lookup, through the descent cache ──────────────────────
                //
                // The fault path, and the only row that touches the Phase 4
                // cache. Addresses are drawn from the live set most of the time
                // and at random the rest, so both the hit and the
                // unmapped-answer paths run.
                uint64_t va;
                if (((r >> 55) & 3) != 0 && live.count != 0) {
                    const unsigned k = static_cast<unsigned>((r >> 41) % live.count);
                    va = live.va[k] + ((r >> 45) % 4) * arch::smallPageSize;
                } else {
                    va = clusterLo + ((r >> 45) % 4096) * kGranule;
                }
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 3
                // Mode 3: the multiplier. Calls made by one lookup, counted
                // rather than inferred, because the descent cache shortens the
                // walk and the harness's per-level figure cannot know by how
                // much on any given lookup.
                const uint64_t c0 = VMS::freshnessCallCount();
                auto result = gCache->lookup(block, va);
                const uint64_t calls = VMS::freshnessCallCount() - c0;
                recordMin(gProbeLookupMin, calls);
                gProbeLookupTicks.fetch_add(calls, RELAXED);
                gProbeLookupCount.fetch_add(1, RELAXED);
#elif defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 2
                // Mode 2: the whole descent, so mode 1 can be stated as a
                // fraction of it. The cache is live here on purpose — this is
                // the cost a fault actually pays, not the cost of a cold walk.
                calibrateProbe();
                const uint64_t l0 = insnCounter();
                auto result = gCache->lookup(block, va);
                const uint64_t l1 = insnCounter();
                recordMin(gProbeLookupMin, l1 - l0);
                gProbeLookupTicks.fetch_add(l1 - l0, RELAXED);
                gProbeLookupCount.fetch_add(1, RELAXED);
#else
                auto result = gCache->lookup(block, va);
#endif
                if (result) {
                    // Touch the record through the reference, which is what
                    // makes this a freshness test and not a pointer-shuffling
                    // one: the record's bytes live in vmsmalloc memory that may
                    // have been another CPU's a moment ago.
                    const uint64_t off = result.mapping()->offsetFor(result.lo());
                    (void)off;
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 1
                    // Mode 1: ONE call, on a pointer the descent just walked to,
                    // bracketed as tightly as the call site allows. Nothing else
                    // is in the bracket, so the delta is the call plus one
                    // rdtsc, and gProbeBase carries the latter away.
                    //
                    // This prices the HIT path deliberately: the page was
                    // touched a moment ago, so its dirty-bitmap word is warm and
                    // the bit is clear. That is the case D-052 shows dominates —
                    // a descent's calls land on 2-3 pages, so all but the first
                    // per page are hits — and it is the one an instruction count
                    // can speak to at all, since a miss is a cache event TCG
                    // does not model.
                    calibrateProbe();
                    const uint64_t f0 = insnCounter();
                    const bool stale =
                        VMS::ensureTLBEntryFresh(result.mapping().address());
                    const uint64_t f1 = insnCounter();
                    if (stale) {
                        recordMin(gProbeStaleMin, f1 - f0);
                        gProbeStaleCount.fetch_add(1, RELAXED);
                    } else {
                        recordMin(gProbeFreshMin, f1 - f0);
                        gProbeFreshTicks.fetch_add(f1 - f0, RELAXED);
                        gProbeFreshCount.fetch_add(1, RELAXED);
                    }
#endif
                }
                gLookups.fetch_add(1, RELAXED);
            }

            gHeartbeat[static_cast<size_t>(me)].beat.store(++beat, RELAXED);
            if ((r >> 60) == 0) {
                // DEC-059's evidence. `drain` reports what it destroyed and
                // `tryAdvance` does not, so the instrumented pump uses it — the
                // sweep is the same work either way; only the epoch-advance
                // attempt differs, and that is not what the bound governs.
                //
                // The question the bound poses is two-sided: unbounded turns a
                // #PF into an arbitrarily long deleter run, too small strands
                // residue behind the pump rate. Both are answered by the same
                // distribution — **does a sweep ever reach the bound?** If the
                // batch never fills, the bound cannot be stranding anything and
                // raising it would change nothing.
                const size_t reclaimed = rcu::drain(block.domain);
                rdx::noteDrainBatch(reclaimed);
            }
        }
    }

    // A deliberate growth, run by CPU 0 once per cycle. Growth over a populated
    // cluster is the ordinary path; growth over an EMPTY one is D-030's accepted
    // residue, and both are reached from here.
    void exerciseGrowth(rdx::KernelBlock& block, uint64_t r) {
        const size_t bucket = static_cast<size_t>((r >> 3) % rdx::kBucketCount);
        const uint64_t lo =
            static_cast<uint64_t>(bucket) << rdx::slotSpanBits(rdx::kAmd64Geometry, 0);
        // Two default cluster spans, so the cluster must root one level higher.
        const uint64_t hi = lo + 2 * rdx::minClusterSpan(rdx::kAmd64Geometry) - 1;
        const bool wasEmpty = !block.clusters.bucketIsOccupied(bucket);
        const auto st = block.clusters.ensureCovers(lo, hi);
        if (st == rdx::ClusterStatus::Ok) {
            gGrowths.fetch_add(1, RELAXED);
            if (wasEmpty) gClusters.fetch_add(1, RELAXED);
        } else if (st == rdx::ClusterStatus::OutOfMemory) {
            gOom.fetch_add(1, RELAXED);
        }
    }

    // ── The residue gate (§11, DEC-096) ────────────────────────────────────
    //
    // Read on a quiesced tree with every CPU parked at the barrier, which is the
    // only condition under which the census's two relaxed counters subtract to
    // an exact figure.
    //
    // The bound has two terms and each is a spec citation rather than a fudge:
    //
    //   - `processorCount() x cacheEntries` — DEC-096's own arithmetic, one
    //     pinned node per descent-cache entry per CPU, "released as each CPU's
    //     cache turns over";
    //   - `2 x clusters` — D-030's accepted residue, where growth over an empty
    //     cluster strands the old root. Two nodes per cluster, recoverable by
    //     the ordinary path, and a gap between two individually-correct rules
    //     rather than a bug.
    //
    // After every CPU has evicted its rows, only the second term may remain. A
    // design that LEAKED the cached nodes rather than pinning them would satisfy
    // the first check and fail this one, which is why both are here.
    void checkResidueAndExitOnFailure(uint64_t pinned, uint64_t evicted, uint64_t mappings) {
        const uint64_t cacheTerm =
            static_cast<uint64_t>(arch::processorCount()) * rdx::kDescentCacheEntries;
        const uint64_t strandedTerm = 2 * gClusters.load(RELAXED);

        if (pinned > cacheTerm + strandedTerm) {
            klog() << "\nradixStress: RESIDUE — " << pinned
                   << " nodes live after teardown with caches held, bound is "
                   << (cacheTerm + strandedTerm) << " (cache " << cacheTerm
                   << " + stranded " << strandedTerm << ")\n";
            exitToHost(ExitStatus::Panic);
        }
        if (evicted > strandedTerm) {
            klog() << "\nradixStress: RESIDUE — " << evicted
                   << " nodes still live after every CPU evicted its cache; only D-030's "
                   << strandedTerm << " stranded roots are allowed. DEC-096's bound is "
                      "supposed to be TEMPORARY, released as each cache turns over.\n";
            exitToHost(ExitStatus::Panic);
        }
        if (mappings != 0) {
            klog() << "\nradixStress: RESIDUE — " << mappings
                   << " Mapping records live after teardown. A leaked record pins its "
                      "VMObject and every frame behind it; the bound here is zero.\n";
            exitToHost(ExitStatus::Panic);
        }
    }

    void liveness(arch::ProcessorID me) {
        klog() << "radixStress: cpu=" << static_cast<uint64_t>(me)
               << " cycles=" << gCycles.load(RELAXED)
               << " place=" << gPlacements.load(RELAXED)
               << " unmap=" << gUnmaps.load(RELAXED)
               << " bulk=" << gBulkUnmaps.load(RELAXED)
               << " dense=" << gDenseRegions.load(RELAXED)
               << " fixed=" << gFixed.load(RELAXED)
               << " lookup=" << gLookups.load(RELAXED)
               << " grow=" << gGrowths.load(RELAXED)
               << " clusters=" << gClusters.load(RELAXED)
               << " noSpace=" << gNoSpace.load(RELAXED)
               << " oom=" << gOom.load(RELAXED)
               << " hit%=" << (gCache->stats().lookups.load(rdx::kCacheAccounting) == 0
                                   ? uint64_t{0}
                                   : 100 * gCache->stats().hits.load(rdx::kCacheAccounting) /
                                         gCache->stats().lookups.load(rdx::kCacheAccounting))
               << " pinAtomics=" << (gCache->stats().pinAcquires.load(rdx::kCacheAccounting) +
                                     gCache->stats().pinReleases.load(rdx::kCacheAccounting))
               << " detEvict=" << gCache->stats().detachedEvictions.load(rdx::kCacheAccounting)
               << " genEvict=" << gCache->stats().generationEvictions.load(rdx::kCacheAccounting)
#ifdef CROCOS_RADIX_NODE_CENSUS
               << " nodesLive=" << rdx::gNodeCensus.liveQuiesced()
               << " residuePinned=" << gResiduePinned.load(RELAXED)
               << " residueEvicted=" << gResidueEvicted.load(RELAXED)
#else
               << " [census=off]"
#endif
#ifdef CROCOS_RADIX_INSN_PROBE
               // D-042/D-052. Ticks are -icount virtual-clock ticks; only the
               // RATIO between modes 1 and 2 is meaningful, and `base` is the
               // empty-probe cost every figure should be read net of.
               << " probeBase=" << gProbeBaseMin.load(RELAXED)
               << " freshMin=" << gProbeFreshMin.load(RELAXED)
               << " freshMean=" << (gProbeFreshCount.load(RELAXED) == 0
                                        ? uint64_t{0}
                                        : gProbeFreshTicks.load(RELAXED) /
                                              gProbeFreshCount.load(RELAXED))
               << " freshN=" << gProbeFreshCount.load(RELAXED)
               << " staleMin=" << gProbeStaleMin.load(RELAXED)
               << " staleN=" << gProbeStaleCount.load(RELAXED)
               << " lookupMin=" << gProbeLookupMin.load(RELAXED)
               << " lookupMean=" << (gProbeLookupCount.load(RELAXED) == 0
                                         ? uint64_t{0}
                                         : gProbeLookupTicks.load(RELAXED) /
                                               gProbeLookupCount.load(RELAXED))
               << " lookupN=" << gProbeLookupCount.load(RELAXED)
#endif
               << "\n";
    }

    // ── The draw histogram (CPU 0, once per liveness print) ────────────────
    //
    // Its own statement rather than fields on the liveness line, because a
    // twelve-bucket distribution is a table and the liveness line is already at
    // the width where a reader stops parsing it. One `klog` statement, so it
    // cannot interleave with another CPU's.
    //
    // Two numbers to read it against since D-059, and they answer different
    // questions. `budget` is the one that BINDS: a draw cannot exceed it, so
    // `max` approaching it means operations are decomposing. `ceiling` is the
    // edge sum an unbudgeted attempt could have wanted — the gap between the two
    // is what the budget saves, and a bucket above it would mean the derivation
    // is wrong.
#ifdef CROCOS_RADIX_DRAW_HISTOGRAM
    void drawHistogram() {
        const auto& h = rdx::gDrawHistogram;
        klog() << "radixStress: DeferredRelease draws/attempt (§7.1) — n="
               << h.total() << " max=" << h.max()
               << " budget=" << static_cast<uint64_t>(rdx::kDefaultRecordsPerCpu)
               << " ceiling=" << static_cast<uint64_t>(rdx::deferredReleaseBound(rdx::kAmd64Geometry))
               << "\n  0:" << h.bucket(0) << " 1:" << h.bucket(1) << " 2:" << h.bucket(2)
               << " 3:" << h.bucket(3) << " 4:" << h.bucket(4) << " 5-8:" << h.bucket(5)
               << " 9-16:" << h.bucket(6) << " 17-32:" << h.bucket(7) << " 33-64:" << h.bucket(8)
               << " 65-128:" << h.bucket(9) << " 129-230:" << h.bucket(10)
               << " OVER-CEILING:" << h.bucket(11) << "\n";
    }

    // DEC-077's evidence, same shape and same population of attempts, so the two
    // distributions can be read against each other.
    void drainHistogram() {
        const auto& h = rdx::gDrainHistogram;
        klog() << "radixStress: objects destroyed per pump (DEC-059) — n=" << h.total()
               << " max=" << h.max()
               << " bound=" << static_cast<uint64_t>(rdx::kDrainBatchBound)
               << "\n  0:" << h.bucket(0) << " 1:" << h.bucket(1) << " 2:" << h.bucket(2)
               << " 3:" << h.bucket(3) << " 4:" << h.bucket(4) << " 5-8:" << h.bucket(5)
               << " 9-16:" << h.bucket(6) << " 17-32:" << h.bucket(7) << " 33-64:" << h.bucket(8)
               << " 65-128:" << h.bucket(9) << " 129-230:" << h.bucket(10)
               << " >230:" << h.bucket(11) << "\n";
    }

    void placementHistogram() {
        const auto& p = rdx::gProbeHistogram;
        const auto& c = rdx::gScanChunkHistogram;
        klog() << "radixStress: placement probes used (DEC-095) — n=" << p.total()
               << " max=" << p.max() << " K=" << static_cast<uint64_t>(rdx::kProbeCount)
               << " fallbacks=" << rdx::gProbeFallbacks.load(rdx::kCensusAccounting)
               << " scanChunkRuns=" << c.total() << " maxChunks=" << c.max()
               << "\n  1:" << p.bucket(1) << " 2:" << p.bucket(2) << " 3:" << p.bucket(3)
               << " 4:" << p.bucket(4) << " 5-8:" << p.bucket(5) << " 9-16:" << p.bucket(6)
               << "\n";
    }

    void detachHistogram() {
        const auto& h = rdx::gDetachHistogram;
        klog() << "radixStress: detachment size in nodes (DEC-077) — n=" << h.total()
               << " max=" << h.max()
               << " budget=" << static_cast<uint64_t>(rdx::kDetachBudget)
               << " decompositions=" << rdx::gDecompositions.load(rdx::kCensusAccounting)
               << "\n  1:" << h.bucket(1) << " 2:" << h.bucket(2)
               << " 3:" << h.bucket(3) << " 4:" << h.bucket(4) << " 5-8:" << h.bucket(5)
               << " 9-16:" << h.bucket(6) << " 17-32:" << h.bucket(7) << " 33-64:" << h.bucket(8)
               << " 65-128:" << h.bucket(9) << " 129-230:" << h.bucket(10)
               << " >230:" << h.bucket(11) << "\n";
    }
#endif

    // ── One-time setup, CPU 0 ──────────────────────────────────────────────
    [[nodiscard]] rdx::KernelBlock* createSpace() {
        rdx::KernelBlock* block = nullptr;
        // `kDefaultRecordsPerCpu`, which is what the kernel will actually ship —
        // this used to pass the 230-record edge sum, i.e. DEC-068's eager sizing,
        // so the stress ran against a population no real address space would get
        // and could not have observed a budget overrun if one existed (D-059).
        const auto st = rdx::createAddressSpace(
            *gStore, numa::DomainID{0}, arch::processorCount(),
            rdx::kDefaultRecordsPerCpu, block);
        if (st != rdx::CreateStatus::Ok) {
            klog() << "\nradixStress: address-space creation failed (ENOMEM)\n";
            exitToHost(ExitStatus::Panic);
        }
        return block;
    }

}   // namespace radix_stress

// Per-CPU stress driver. Runs until the shutdown timer fires; a panic, a hang or
// a residue-bound violation is the failure.
[[noreturn]] void radixStress() {
    namespace st = radix_stress;
    namespace rdx = kernel::mm::radix;

    const arch::ProcessorID me = getLogicalProcessorID();
    const size_t cpus = arch::processorCount();
    assert(static_cast<size_t>(me) < st::kMaxCpus,
           "radixStress: CPU id exceeds kMaxCpus — raise the bound");
    assert(rcu::kernelDomain.initialized(),
           "radixStress: RCU is not up — check .icd phase ordering");

    uint64_t beat = 0;
    uint64_t localSense = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull ^
                   (uint64_t{static_cast<uint64_t>(me)} * 0xBF58476D1CE4E5B9ull);

    if (me == 0) {
        // §10's requirement, and the first time it can run: "each codec must
        // assert its own base and range at construction rather than trust
        // vas-layout.md to stay true."
        rdx::verifyKernelCodecBase();

        st::gStore = new (st::gStoreStorage) rdx::KernelBlocks();
        st::gAssign   = new (st::gAssignStorage) rdx::PerCpuAssignment();
        st::gCache    = new (st::gCacheStorage) rdx::KernelCache();
        klog() << "radixStress: cache row is " << sizeof(rdx::KernelCache) / arch::processorCount()
               << " B/CPU nominal, " << sizeof(rdx::KernelCache) << " B total\n";
        st::gSpace.store(st::createSpace(), RELEASE);
        timing::enqueueEvent([] { st::watchdogTick(); }, st::kWatchdogFirstMs);
    }

    klog() << "radixStress: starting on CPU " << static_cast<uint64_t>(me) << "\n";

    while (st::gSpace.load(ACQUIRE) == nullptr) {
        st::gHeartbeat[static_cast<size_t>(me)].beat.store(++beat, RELAXED);
        tight_spin();
    }

    for (;;) {
        rdx::KernelBlock* block = st::gSpace.load(ACQUIRE);
        st::churn(*block, me, rng, beat);
        if (me == 0) st::exerciseGrowth(*block, rng);

        st::barrierWait(cpus, me, localSense, beat);

        // ── The measurement window ─────────────────────────────────────────
        //
        // Everyone is parked, so the tree is quiesced and the census is exact.
        // CPU 0 tears down; then every CPU evicts its own descent-cache row —
        // which cannot be done for it, because the rows are per-CPU and forcing
        // it would take exactly the cross-CPU IPI the project bars from
        // correctness duty (DEC-096).
        if (me == 0) {
            rdx::destroyAddressSpace(*st::gStore, block);
#ifdef CROCOS_RADIX_NODE_CENSUS
            st::gResiduePinned.store(rdx::gNodeCensus.liveQuiesced(), RELAXED);
#endif
        }
        st::barrierWait(cpus, me, localSense, beat);

        st::gCache->evictAllOnThisCpu();
        for (unsigned i = 0; i < st::kLiveSlots; i++) {
            st::gLive[static_cast<size_t>(me)].va[i] = 0;
        }
        st::gLive[static_cast<size_t>(me)].count = 0;

        st::barrierWait(cpus, me, localSense, beat);

        if (me == 0) {
#ifdef CROCOS_RADIX_NODE_CENSUS
            const uint64_t evicted = rdx::gNodeCensus.liveQuiesced();
            const uint64_t records = rdx::gMappingCensus.liveQuiesced();
            st::gResidueEvicted.store(evicted, RELAXED);
            st::checkResidueAndExitOnFailure(st::gResiduePinned.load(RELAXED), evicted, records);
#endif
            st::gAssign->~PerCpuAssignment();
            st::gAssign = new (st::gAssignStorage) rdx::PerCpuAssignment();
            st::gSpace.store(st::createSpace(), RELEASE);
            const uint64_t cycle = st::gCycles.fetch_add(1, RELAXED) + 1;
            if ((cycle & st::kLivenessMask) == 1 || cycle <= 4) {
                st::liveness(me);
#ifdef CROCOS_RADIX_DRAW_HISTOGRAM
                st::drawHistogram();
                st::detachHistogram();
                st::drainHistogram();
                st::placementHistogram();
#endif
            }
        }
        st::barrierWait(cpus, me, localSense, beat);
    }
}

}   // namespace kernel
