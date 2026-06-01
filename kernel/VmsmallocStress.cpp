//
// vmsmalloc Phase 9 — in-kernel stress test.
//
// Replaces the old VMSubstrate::allocPage / freePage `naiveTest` as the
// smp_bringup per-CPU stress routine (registered as `[VmsmallocStress]` in
// kernel/general.icd). It drives the production allocator against real kernel
// VAs on every CPU: each iteration allocates a batch across all slab size
// classes plus a DEC-029 whole-page bucket, writes a deterministic
// (cpu, class, index, iteration) content pattern, reads it back to catch
// corruption / use-after-free / magazine-state confusion, then frees — with
// ~10% of allocations handed off to another CPU so the cross-domain free gate
// (DEC-019) is exercised. It runs forever; the absence of a panic is the pass
// signal. This is the canonical validator for DEC-015's packed-tagged-head
// address math against real kernel VAs (Phase 8's userspace harness deferred
// it). See specs/vmsmalloc-phase-9.md.
//
// DEC-028 discipline: vmsmalloc is *never* used raw here. Every allocation goes
// through VMSubstrate::make<T> and every read of allocator-returned memory goes
// through a SafePtr<T>, whose operator* / operator-> call ensureTLBEntryFresh
// before the dereference — the read-side half of the eager-free stale-TLB fix
// (the allocator-side half is DEC-047's reclaimSlabPage sentinel; see
// docs/vmsmalloc-stale-tlb-bug.md). Untyped slots are modeled as Blob<Bytes>;
// the hand-off inbox lives in BSS (not slab-allocated intrusive nodes) so its
// internal links are never read cross-CPU through a stale TLB entry.
//

#include <kernel.h>
#include <arch.h>
#include <mem/VMSubstrate.h>
#include <mem/NUMA.h>
#include <core/atomic.h>

namespace VMS  = kernel::mm::VMSubstrate;   // make / destroy / SafePtr
namespace slab = kernel::mm::vmsmalloc;     // kSlabSizeClasses / kNumSizeClasses

namespace kernel {

    // ── Tunables (P9-DEC-002/004/005/006) ──────────────────────────────────
    //
    // kAllocsPerClass live allocations per class per iteration; kPoolClasses
    // is one slot per slab class plus a trailing DEC-029 whole-page bucket.
    // kStressMaxCpus bounds the static pool / inbox arrays — matches the CPU
    // counts used by the run targets (8) and qmon (16). A loud entry assert
    // guards against a larger -smp.
    static constexpr size_t        kAllocsPerClass = 256;
    static constexpr size_t        kPoolClasses    = slab::kNumSizeClasses + 1;
    static constexpr size_t        kWholePageBytes = 1024;   // > 512 B largest class → DEC-029 bypass
    static constexpr size_t        kStressMaxCpus  = 16;
    static constexpr uint64_t      kLivenessMask   = 0xFull;  // klog every 64K iters (and iter 0)

    namespace vmsmalloc::stress {

        // Byte size for a pool bucket: the size class, or the whole-page bucket.
        // constexpr so it can supply the compile-time Bytes in dispatchOnBucket.
        static constexpr size_t bucketSize(size_t c) {
            return (c < slab::kNumSizeClasses) ? slab::kSlabSizeClasses[c]
                                               : kWholePageBytes;
        }

        // A fixed-size typed slot. make<Blob<Bytes>> drives class selection by
        // sizeof; SafePtr<Blob<Bytes>> makes every read TLB-fresh. Every current
        // bucket (8..512, 1024) is a multiple of 8.
        template <size_t Bytes>
        struct Blob {
            static_assert(Bytes % 8 == 0, "Blob size must be a multiple of 8");
            uint64_t w[Bytes / 8];
        };

        // Map a runtime bucket index to its compile-time Bytes and invoke a
        // template lambda `f.operator()<Bytes>()`. Sibling of vmsmalloc.cpp's
        // dispatchOnClass; the trailing case is the DEC-029 whole-page bucket.
        template <typename F>
        inline void dispatchOnBucket(size_t c, F&& f) {
#define VMS_STRESS_CASE(idx) case idx: f.template operator()<bucketSize(idx)>(); return;
            switch (c) {
                VMS_STRESS_CASE(0)
                VMS_STRESS_CASE(1)
                VMS_STRESS_CASE(2)
                VMS_STRESS_CASE(3)
                VMS_STRESS_CASE(4)
                VMS_STRESS_CASE(5)
                VMS_STRESS_CASE(6)
                VMS_STRESS_CASE(7)
                VMS_STRESS_CASE(8)   // whole-page bucket (kNumSizeClasses == 8)
                default:
                    assert(false, "vmsmallocStress: dispatchOnBucket invalid bucket");
            }
#undef VMS_STRESS_CASE
        }
        static_assert(slab::kNumSizeClasses == 8,
                      "vmsmallocStress dispatch assumes exactly 8 slab classes + 1 whole-page");

        // Pack (cpu, class, index, iteration) into 8 B (P9-DEC-002). The fields
        // are disjoint: cpu→[48..55], class→[40..47], index→[24..39],
        // iteration→[0..23] (wraps every 16M iters, beyond any run).
        inline uint64_t packPattern(uint32_t cpu, uint32_t classIdx,
                                    uint32_t index, uint64_t iteration) {
            return (static_cast<uint64_t>(cpu)      << 48)
                 | (static_cast<uint64_t>(classIdx) << 40)
                 | (static_cast<uint64_t>(index)    << 24)
                 | (iteration & 0xFFFFFFull);
        }

        // Stamp the 8 B pattern across a slot through its SafePtr (P9-ITEM-002).
        // The operator-> does the one load-bearing ensureTLBEntryFresh; the
        // volatile view over the returned pointer keeps the round-trip from
        // being optimized away. One freshness per episode is sufficient — a live
        // allocation's slab is never eager-freed, so its PTE/TLB entry is stable
        // for the duration of the loop.
        template <size_t Bytes>
        void fillBlob(void* raw, uint64_t v) {
            VMS::SafePtr<Blob<Bytes>> sp{static_cast<Blob<Bytes>*>(raw)};
            volatile uint64_t* w = sp->w;
            for (size_t i = 0; i < Bytes / 8; i++) w[i] = v;
        }

        // Verify the pattern through the SafePtr; panic with full context on a
        // mismatch (the canonical corruption / stale-content / UAF trip-wire).
        template <size_t Bytes>
        void verifyBlob(void* raw, uint64_t v, uint32_t cpu, uint32_t classIdx,
                        uint32_t index, uint64_t iteration) {
            VMS::SafePtr<Blob<Bytes>> sp{static_cast<Blob<Bytes>*>(raw)};
            volatile uint64_t* w = sp->w;
            for (size_t i = 0; i < Bytes / 8; i++) {
                const uint64_t got = w[i];
                if (got != v) {
                    klog() << "vmsmallocStress: CONTENT MISMATCH cpu=" << cpu
                           << " class=" << classIdx << " index=" << index
                           << " iter=" << iteration << " word=" << static_cast<uint64_t>(i)
                           << " ptr=" << raw << " expected=" << v << " got=" << got << "\n";
                    assert(got == v, "vmsmallocStress content verification failed");
                }
            }
        }

        // Free a slot, reconstructing its typed SafePtr so destroy<T> runs the
        // dtor through the freshness path and hands the right size back.
        template <size_t Bytes>
        void destroyBlob(void* raw) {
            VMS::destroy(VMS::SafePtr<Blob<Bytes>>{static_cast<Blob<Bytes>*>(raw)});
        }

        // ~10% hand-off rate, deterministic in (iteration, class, index) so a
        // failure trace is reproducible at the same iteration count (P9-DEC-004).
        inline bool shouldHandoff(uint64_t iteration, size_t classIdx, size_t index) {
            return ((iteration * 31 + classIdx * 17 + index * 7) % 100) < 10;
        }

        // Prefer a recipient on a *different* NUMA domain so the cross-domain
        // free gate is actually exercised on run_numa / run_numa_hmat. On a
        // single-domain config no such CPU exists and we return myCpu, which
        // degenerates the hand-off to a local free (P9-ITEM-004).
        arch::ProcessorID pickRecipientCpu(arch::ProcessorID myCpu) {
            const size_t n = arch::processorCount();
            const numa::DomainID myDomain = numa::numaPolicy().homeDomain(myCpu);
            for (size_t off = 1; off < n; off++) {
                const auto cand = static_cast<arch::ProcessorID>((myCpu + off) % n);
                if (numa::numaPolicy().homeDomain(cand) != myDomain) return cand;
            }
            return myCpu;
        }

    }  // namespace vmsmalloc::stress

    // ── Cross-domain hand-off plumbing (P9-DEC-003) ─────────────────────────
    //
    // The carrier is still slab-allocated (recursive stress: make<HandoffEntry>)
    // but the inbox is a per-CPU fixed-capacity BSS array of Atomic carrier
    // pointers — *not* an intrusive list whose prev/next live on slab pages.
    // Donors publish a carrier into a free slot with a RELEASE CAS; the single
    // consumer (the owning CPU) ACQUIRE-loads it and reads the carrier and its
    // donated payload through SafePtr, so no slab-allocated memory is ever read
    // cross-CPU through a raw (possibly stale-TLB) pointer.
    struct HandoffEntry {
        void*    ptr       = nullptr;   // the donated Blob<Bytes> allocation
        uint32_t cpu       = 0;         // donor identity — recomputes the pattern
        uint32_t classIdx  = 0;         // bucket → Bytes via dispatchOnBucket
        uint32_t index     = 0;
        uint64_t iteration = 0;
    };

    static constexpr size_t kInboxCap = 512;   // per-CPU slot count; overflow → local free

    // BSS, zero-initialised. Static pools mirror naiveTest's page_pools (large
    // per-CPU pools have overflowed the kernel stack before).
    static void*                   stressPools[kStressMaxCpus][kPoolClasses][kAllocsPerClass];
    static Atomic<HandoffEntry*>   handoffInbox[kStressMaxCpus][kInboxCap];

    namespace vmsmalloc::stress {

        // Publish a carrier into a recipient's inbox. Returns false if full.
        // RELEASE on success publishes the carrier's fields (and the donated
        // payload's content) to the recipient's ACQUIRE load.
        inline bool postHandoff(arch::ProcessorID r, HandoffEntry* e) {
            for (size_t s = 0; s < kInboxCap; s++) {
                if (handoffInbox[r][s].compare_exchange_v(nullptr, e, RELEASE, RELAXED))
                    return true;
            }
            return false;
        }

        // Donor: allocate a carrier, fill it through its SafePtr, and publish.
        // On a full inbox, free the donated allocation locally so the donor
        // always makes progress.
        void enqueueHandoff(arch::ProcessorID r, void* dptr, uint32_t cpu,
                            uint32_t classIdx, uint32_t index, uint64_t iteration) {
            auto e = VMS::make<HandoffEntry>();
            e->ptr = dptr; e->cpu = cpu; e->classIdx = classIdx;
            e->index = index; e->iteration = iteration;
            if (!postHandoff(r, static_cast<HandoffEntry*>(e.raw()))) {
                VMS::destroy(e);
                dispatchOnBucket(classIdx, [&]<size_t Bytes>{ destroyBlob<Bytes>(dptr); });
            }
        }

        // Consumer: drain our own inbox — verify each donated payload against
        // the donor's pattern, free it, then free the carrier.
        void drainInbox(arch::ProcessorID myCpu) {
            for (size_t s = 0; s < kInboxCap; s++) {
                HandoffEntry* raw = handoffInbox[myCpu][s].load(ACQUIRE);
                if (raw == nullptr) continue;
                handoffInbox[myCpu][s].store(nullptr, RELAXED);  // single consumer

                VMS::SafePtr<HandoffEntry> ep{raw};
                const HandoffEntry h = *ep;   // one freshness call, copy fields out
                const uint64_t v = packPattern(h.cpu, h.classIdx, h.index, h.iteration);
                dispatchOnBucket(h.classIdx, [&]<size_t Bytes>{
                    verifyBlob<Bytes>(h.ptr, v, h.cpu, h.classIdx, h.index, h.iteration);
                    destroyBlob<Bytes>(h.ptr);
                });
                VMS::destroy(ep);
            }
        }

    }  // namespace vmsmalloc::stress

    // Per-CPU stress driver — runs forever (its own success signal). Registered
    // at smp_bringup, so every CPU runs its own instance.
    [[noreturn]] bool vmsmallocStress() {
        namespace stress = vmsmalloc::stress;
        const arch::ProcessorID myCpu = arch::getCurrentProcessorID();
        assert(myCpu < kStressMaxCpus,
               "vmsmallocStress: CPU id exceeds kStressMaxCpus — raise the bound");
        klog() << "vmsmallocStress: starting on CPU " << myCpu << "\n";

        auto& pools = stressPools[myCpu];
        uint64_t iteration = 0;

        while (true) {
            // 1. Drain hand-off inbox: verify + free what other CPUs gave us.
            stress::drainInbox(myCpu);

            // 2. Allocate a fresh batch across all classes + whole-page, and
            //    stamp each slot with its (cpu, class, index, iteration) pattern.
            for (size_t c = 0; c < kPoolClasses; c++) {
                for (size_t i = 0; i < kAllocsPerClass; i++) {
                    const uint64_t v = stress::packPattern(
                        myCpu, static_cast<uint32_t>(c),
                        static_cast<uint32_t>(i), iteration);
                    stress::dispatchOnBucket(c, [&]<size_t Bytes>{
                        auto sp = VMS::make<stress::Blob<Bytes>>();
                        pools[c][i] = sp.raw();
                        volatile uint64_t* w = sp->w;
                        for (size_t k = 0; k < Bytes / 8; k++) w[k] = v;
                    });
                }
            }

            // 3. Verify every slot before freeing anything (catches cross-
            //    allocation writes and stale-content reads).
            for (size_t c = 0; c < kPoolClasses; c++) {
                for (size_t i = 0; i < kAllocsPerClass; i++) {
                    const uint64_t v = stress::packPattern(
                        myCpu, static_cast<uint32_t>(c),
                        static_cast<uint32_t>(i), iteration);
                    stress::dispatchOnBucket(c, [&]<size_t Bytes>{
                        stress::verifyBlob<Bytes>(pools[c][i], v, myCpu,
                            static_cast<uint32_t>(c), static_cast<uint32_t>(i), iteration);
                    });
                }
            }

            // 4. Free locally, or hand ~10% off to another (preferably cross-
            //    domain) CPU. A self-recipient just frees locally.
            for (size_t c = 0; c < kPoolClasses; c++) {
                for (size_t i = 0; i < kAllocsPerClass; i++) {
                    void* p = pools[c][i];
                    if (stress::shouldHandoff(iteration, c, i)) {
                        const arch::ProcessorID r = stress::pickRecipientCpu(myCpu);
                        if (r == myCpu) {
                            stress::dispatchOnBucket(c, [&]<size_t Bytes>{ stress::destroyBlob<Bytes>(p); });
                        } else {
                            stress::enqueueHandoff(r, p, myCpu,
                                static_cast<uint32_t>(c), static_cast<uint32_t>(i), iteration);
                        }
                    } else {
                        stress::dispatchOnBucket(c, [&]<size_t Bytes>{ stress::destroyBlob<Bytes>(p); });
                    }
                    pools[c][i] = nullptr;
                }
            }

            // 5. Periodic liveness (fires at iter 0 too — confirms each CPU
            //    entered the loop). Silence > 30s ⇒ hang.
            if ((iteration & kLivenessMask) == 0) {
                klog() << "vmsmallocStress: CPU " << myCpu
                       << " iter=" << iteration << "\n";
            }
            iteration++;
        }
    }

}  // namespace kernel
