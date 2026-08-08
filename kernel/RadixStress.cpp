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
    constexpr unsigned kOpsPerCycle   = 512;

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
    alignas(rdx::KernelBlocks) unsigned char gFreelistStorage[sizeof(rdx::KernelBlocks)];
    alignas(rdx::KernelCache)  unsigned char gCacheStorage[sizeof(rdx::KernelCache)];
    alignas(rdx::PerCpuAssignment)
        unsigned char gAssignStorage[sizeof(rdx::PerCpuAssignment)];

    rdx::KernelBlocks*      gFreelist = nullptr;
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
    Atomic<uint64_t> gOom{0};
    Atomic<uint64_t> gGrowths{0};
    Atomic<uint64_t> gClusters{0};        // D-030's stranded-root allowance is 2x this
    Atomic<uint64_t> gCycles{0};
    Atomic<uint64_t> gResiduePinned{0};   // last cycle's, with caches held
    Atomic<uint64_t> gResidueEvicted{0};  // ...and after every CPU evicted

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

    void reportHangAndExit(size_t stalled, size_t cpus) {
        klog() << "\nradixStress: WATCHDOG — cpu=" << static_cast<uint64_t>(stalled)
               << " made no progress across " << static_cast<uint64_t>(kStallSamples)
               << " samples (" << (kStallSamples * kWatchdogPeriodMs) << " ms)\n";
        for (size_t i = 0; i < cpus && i < kMaxCpus; ++i) {
            const uint64_t b = gHeartbeat[i].beat.load(RELAXED);
            klog() << "  cpu=" << static_cast<uint64_t>(i) << " beat=";
            if (b == 0) klog() << "(never started)\n"; else klog() << b << "\n";
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
            } else if (row < 14) {
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
                auto result = gCache->lookup(block, va);
                if (result) {
                    // Touch the record through the reference, which is what
                    // makes this a freshness test and not a pointer-shuffling
                    // one: the record's bytes live in vmsmalloc memory that may
                    // have been another CPU's a moment ago.
                    const uint64_t off = result.mapping()->offsetFor(result.lo());
                    (void)off;
                }
                gLookups.fetch_add(1, RELAXED);
            }

            gHeartbeat[static_cast<size_t>(me)].beat.store(++beat, RELAXED);
            if ((r >> 60) == 0) (void)rcu::tryAdvance(block.domain);
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
               << "\n";
    }

    // ── One-time setup, CPU 0 ──────────────────────────────────────────────
    [[nodiscard]] rdx::KernelBlock* createSpace() {
        rdx::KernelBlock* block = nullptr;
        const auto st = rdx::createAddressSpace(
            *gFreelist, numa::DomainID{0}, arch::processorCount(),
            rdx::deferredReleaseBound(rdx::kAmd64Geometry), block);
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

        st::gFreelist = new (st::gFreelistStorage) rdx::KernelBlocks();
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
            rdx::destroyAddressSpace(*st::gFreelist, block);
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
            if ((cycle & st::kLivenessMask) == 1 || cycle <= 4) st::liveness(me);
        }
        st::barrierWait(cpus, me, localSense, beat);
    }
}

}   // namespace kernel
