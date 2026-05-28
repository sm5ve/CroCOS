//
// vmsmalloc Phase 5 — the `vmsmalloc(size)` happy path.
//
// Implements kernel::mm::VMSubstrate::vmsmalloc (declared in
// kernel/include/mem/VMSubstrate.h). Covers:
//   - DEC-029 whole-page bypass for sizes above the largest slab class.
//   - DEC-037 unified-magazine fast path with the DEC-039 pre-read of
//     `chainNext` before `allocSlot`.
//   - DEC-034 / DEC-041 chained-Treiber refill via `partialFor(d, c)->pop()`,
//     with DEC-040 (amended) lazy first-touch TLB freshness: the load-bearing
//     pop-internal call lives in Phase 4's `pop()` via the VmsmallocHooks
//     onPreTouch hook (P4-DEC-010); Phase 5 adds an `ensureTLBEntryFresh`
//     call at every subsequent `m.head` transition.
//   - DEC-036 bounded eager-free walk of Empty chain heads above the
//     single-slab floor.
//   - DEC-018 fresh-slab slow path (allocPage + descriptor init + bookkeeper
//     seed), with the home NUMA domain recorded at creation.
//
// The DEC-015 packed-tagged-head encoding lands here as the vmsmalloc-supplied
// `VmsHeadEncoding` policy for Core::ChainedTreiberStack. Its bit budget is
// derived from arch::pageTableDescriptor (the VMSubstrate VA window is a single
// top-level page-table entry), not hardcoded.
//
// Per P5-DEC-001 the 8-way SlabDescriptor<N> dispatch is an inline switch.
// Edge-input asserts (size == 0, IRQ/NMI context) and the vmsfree body are
// Phase 7 / Phase 6; Phase 5 carries only the load-bearing `size <= pageSize`
// guard on the whole-page branch.
//

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <kassert.h>
#include <kernel.h>
#include <kmemlayout.h>
#include <CpuLocal.h>
#include <interrupts/InterruptContextDepths.h>
#include <core/atomic.h>
#include <core/atomic/TreiberStack.h>
#include <core/math.h>
#include <core/mem.h>
#include <core/utility.h>
#include <liballoc/OccupancyTransition.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>

#include "VMSubstrateSlab.h"

namespace kernel::mm::vmsmalloc {

namespace {

// ─── DEC-015 head encoding constants (derived, not hardcoded) ──────────────
//
// The VMSubstrate VA window occupies one entry at the top of its page-table
// subtree (PML4[VMM_SUBSTRATE_ROOT_INDEX] on AMD64). The span of that entry,
// in bits, is getVirtualAddressBitCount(pageTableLevelForKMemRegion() - 1) —
// the same shift arenaVirtualBase uses to position the window. Subtracting
// the small-page shift gives the number of page-offset bits the encoding must
// carry; the remaining bits of the 64-bit head are the ABA counter.
inline constexpr unsigned kVmsRegionVABits =
    arch::pageTableDescriptor.getVirtualAddressBitCount(
        kernel::mm::pageTableLevelForKMemRegion() - 1);
inline constexpr unsigned kPageShift = log2floor(arch::smallPageSize);

// File-scope VA window, published once by vmsmallocLateInit and immutable
// thereafter (P5-ITEM-004). Read without atomics — no concurrent writers
// post-init. `vmsBase` is arenaVirtualBase(0); offset 0 (== vmsBase) is the
// reserved empty-stack marker and can never be a descriptor, because arena 0's
// allocatable region starts well above its base (self-ref + occupancy buffer
// + CpuLocal page precede it). `vmsSize` feeds the P5-DEC-003 runtime check.
uintptr_t vmsBase = 0;
size_t    vmsSize = 0;

// DEC-015 packed-tagged-head encoding policy for Core::ChainedTreiberStack.
// head = (counter << kOffsetBits) | descPageOffset.
struct VmsHeadEncoding {
    using Storage = uint64_t;
    using Tag     = uint64_t;

    static constexpr unsigned kOffsetBits  = kVmsRegionVABits - kPageShift;
    static constexpr unsigned kCounterBits = 64u - kOffsetBits;
    static constexpr Tag      kCounterMask = (Tag{1} << kCounterBits) - 1;
    static constexpr Storage  kOffsetMask  = (Storage{1} << kOffsetBits) - 1;

    static Storage pack(SlabDescriptorBase* p, Tag t) {
        if (p == nullptr) return (t & kCounterMask) << kOffsetBits;  // empty stack
        const uintptr_t off =
            (reinterpret_cast<uintptr_t>(p) - vmsBase) >> kPageShift;
        return ((t & kCounterMask) << kOffsetBits) | (off & kOffsetMask);
    }
    static SlabDescriptorBase* unpackPointer(Storage s) {
        const Storage off = s & kOffsetMask;
        if (off == 0) return nullptr;  // offset 0 reserved for the empty marker
        const uintptr_t va = vmsBase + (off << kPageShift);
        return reinterpret_cast<SlabDescriptorBase*>(va);
    }
    static Tag advanceTag(Tag t) { return (t + 1) & kCounterMask; }  // push-only, masked
    static Tag unpackTag(Storage s) { return (s >> kOffsetBits) & kCounterMask; }
};

// Zero-init Storage decodes to the empty pointer (TreiberHeadEncoding
// contract): unpackPointer(0) reads off == 0 and returns nullptr without
// touching vmsBase. Not static_assert-able (unpackPointer is not constexpr —
// it references the runtime vmsBase global), but holds by inspection of the
// off == 0 early-out above.
//
// The page-offset field exactly spans the VMSubstrate VA window.
static_assert(VmsHeadEncoding::kOffsetBits + VmsHeadEncoding::kCounterBits == 64,
              "DEC-015: offset + counter must fill the 64-bit head");
static_assert(((uintptr_t{1} << VmsHeadEncoding::kOffsetBits) << kPageShift)
                  >= (uintptr_t{1} << kVmsRegionVABits),
              "DEC-015: offset bit budget must cover the VMSubstrate VA window");

// ─── Linkage extractors for Core::ChainedTreiberStack ──────────────────────
struct SlabNextLinkage {
    static SlabDescriptorBase* getNext(SlabDescriptorBase& n) { return n.next; }
    static void setNext(SlabDescriptorBase& n, SlabDescriptorBase* p) { n.next = p; }
};

struct SlabChainLinkage {
    // chainNext is Atomic; DEC-042 #3 pins these reads/writes to RELAXED — the
    // cross-CPU happens-before edge comes from the popper's ACQUIRE-load.
    static SlabDescriptorBase* getChainNext(SlabDescriptorBase& n) {
        return n.chainNext.load(RELAXED);
    }
    static void setChainNext(SlabDescriptorBase& n, SlabDescriptorBase* p) {
        n.chainNext.store(p, RELAXED);
    }
    static uint32_t getChainDepth(SlabDescriptorBase& n) { return n.chainDepth; }
    static void setChainDepth(SlabDescriptorBase& n, uint32_t d) { n.chainDepth = d; }
};

// ─── Tuning + freshness Hooks (P4-DEC-010 / P5-DEC-007) ────────────────────
//
// Bundles the load-bearing pop-internal TLB freshness call (onPreTouch) with
// the tuning-counter bumps. Each (domain, class) stack carries a Hooks bound
// to that slot's MagazineTuning row.
struct VmsmallocHooks {
    MagazineTuning* tuning;

    template <typename T>
    void onPreTouch(T* topPtr) const {
        // DEC-040 amended: load-bearing pop-internal freshness call. Phase 4's
        // pop() fires this AFTER the head acquire-load but BEFORE any read of
        // *topPtr (topPtr->next is republished as the new shared head, so a
        // TLB-stale read would corrupt the stack for all CPUs).
        VMSubstrate::ensureTLBEntryFresh(topPtr);
    }
    void onCasFailure() const { tuning->overflowCount.fetch_add(1, RELAXED); }
    void onEmptyStack() const { tuning->starvationCount.fetch_add(1, RELAXED); }
};

// The per-(domain, class) shared stack, parameterized by vmsmalloc's policies.
using PartialStack = Core::ChainedTreiberStack<
    SlabDescriptorBase, SlabNextLinkage, SlabChainLinkage,
    VmsHeadEncoding, VmsmallocHooks>;

static_assert(sizeof(PartialStack) <= sizeof(PartialStackStorage),
              "PartialStack must fit in PartialStackStorage");
static_assert(alignof(PartialStack) <= alignof(PartialStackStorage),
              "PartialStack alignment must fit PartialStackStorage");

// Recover the typed stack for (domain, class) from its opaque storage. The
// instance was placement-new'd by vmsmallocLateInit; launder defeats the
// compiler's assumption that the storage still holds raw bytes.
inline PartialStack* partialFor(numa::DomainID d, size_t c) {
    return launder(reinterpret_cast<PartialStack*>(&partialStorageFor(d)[c]));
}

// ─── 8-way SlabDescriptor<N> dispatch (P5-DEC-001) ─────────────────────────
//
// Calls `f(static_cast<SlabDescriptor<slotCount(c)>*>(d))`. Used for reading a
// runtime-class slab (allocSlot on the fast path, isEmpty in the eager-free
// walk). Sibling of createFreshSlab's construction switch — one per use, no
// shared helper beyond this thin invoker.
template <typename F>
inline auto dispatchOnClass(size_t c, SlabDescriptorBase* d, F&& f) {
    // slotCount(size_t{idx}) — the size_t brace disambiguates from the
    // SlabDescriptorBase* overload (literal 0 is otherwise a null-pointer
    // constant matching both).
#define VMS_DISPATCH_CASE(idx) \
    case idx: return f(static_cast<SlabDescriptor<slotCount(size_t{idx})>*>(d));
    switch (c) {
        VMS_DISPATCH_CASE(0)
        VMS_DISPATCH_CASE(1)
        VMS_DISPATCH_CASE(2)
        VMS_DISPATCH_CASE(3)
        VMS_DISPATCH_CASE(4)
        VMS_DISPATCH_CASE(5)
        VMS_DISPATCH_CASE(6)
        VMS_DISPATCH_CASE(7)
        default: PANIC("vmsmalloc: dispatchOnClass invalid class ", static_cast<uint64_t>(c));
    }
#undef VMS_DISPATCH_CASE
    __builtin_unreachable();
}

static_assert(kNumSizeClasses == 8,
              "vmsmalloc dispatch switches assume exactly 8 size classes (P5-DEC-001)");

inline uint8_t* slotZeroAddr(SlabDescriptorBase* d, size_t c) {
    return reinterpret_cast<uint8_t*>(d) + slot0Offset(c);
}
// Convenience overload reading the descriptor's own class (the freer path
// recovers `desc` from the page base and does not carry `c` separately).
inline uint8_t* slotZeroAddr(SlabDescriptorBase* d) {
    return slotZeroAddr(d, d->sizeClass);
}

// ─── Slow-path slab creation (DEC-018 / DEC-044 / P5-DEC-006) ──────────────
SlabDescriptorBase* createFreshSlab(arch::ProcessorID i, size_t c) {
    void* page = VMSubstrate::allocPage();  // panics on failure (DEC-012)
    // DEC-018: record the creating CPU's home domain. May diverge from the
    // physical page placement under local exhaustion — parent spec accepts.
    const numa::DomainID home = numa::numaPolicy().homeDomain(i);
    SlabDescriptorBase* base = nullptr;
#define VMS_CREATE_CASE(idx)                                                   \
    case idx: {                                                                \
        auto* d = new (page) SlabDescriptor<slotCount(size_t{idx})>();         \
        base = d;                                                              \
        /* P5-DEC-006: magic first, then the rest of the prefix, then the */   \
        /* bookkeeper seed. Same-CPU program order; the flush's release-CAS */ \
        /* (Phase 6) handles cross-CPU publication. */                        \
        base->magic       = kSlabDescriptorMagic;                              \
        base->next        = nullptr;                                           \
        base->chainNext.store(nullptr, RELAXED);                               \
        base->chainDepth  = 1;                                                 \
        base->numaDomain  = home;                                              \
        base->sizeClass   = static_cast<uint8_t>(idx);                         \
        base->_padding[0] = 0;                                                 \
        d->bookkeeper.seedAllAvailable(slotCount(size_t{idx})); /* DEC-011 mask */\
        break;                                                                 \
    }
    switch (c) {
        VMS_CREATE_CASE(0)
        VMS_CREATE_CASE(1)
        VMS_CREATE_CASE(2)
        VMS_CREATE_CASE(3)
        VMS_CREATE_CASE(4)
        VMS_CREATE_CASE(5)
        VMS_CREATE_CASE(6)
        VMS_CREATE_CASE(7)
        default: PANIC("vmsmalloc: createFreshSlab invalid class ", static_cast<uint64_t>(c));
    }
#undef VMS_CREATE_CASE
    return base;
}

// ─── Phase 6 freer-side publish helpers (P6-DEC-003) ───────────────────────

// Recover a slot index from a slab-backed pointer. Caller has validated p is
// within the slab's data range and slot-aligned.
inline size_t slotIndexOf(void* p, SlabDescriptorBase* desc) {
    const uintptr_t off = reinterpret_cast<uintptr_t>(p)
                        - reinterpret_cast<uintptr_t>(slotZeroAddr(desc));
    return off / slotSize(desc);
}

// Cross-domain publish (DEC-019 gate, P6-DEC-002 / user direction 2026-05-27).
// Phase-4 push(element) may extend the home-domain top chain (if below
// maxChainLength) or start a new singleton — favoring shared-stack
// amortization over an always-singleton publish.
inline void crossDomainPublish(SlabDescriptorBase* desc) {
    partialFor(desc->numaDomain, desc->sizeClass)->push(*desc);
}

// Same-domain publish (DEC-034). Prepend desc to the local magazine (CPU-
// private; no atomics on m.head / m.depth per DEC-014/030); when the depth
// reaches the stack's maxChainLength, flush the whole chain to the shared
// stack via pushChain.
inline void sameDomainPublishAndMaybeFlush(arch::ProcessorID i,
                                           SlabDescriptorBase* desc) {
    const size_t c = desc->sizeClass;
    Magazine& m = kernel::cpuLocal().magazines[c];  // P7-DEC-010
    // Extend at head. chainNext is Atomic for C++ data-race freedom (DEC-042
    // #3); RELAXED compiles to a plain store on AMD64/ARMv8.
    desc->chainNext.store(m.head, RELAXED);
    m.head = desc;
    m.depth++;

    PartialStack* stack = partialFor(numa::numaPolicy().homeDomain(i), c);
    if (m.depth >= stack->getMaxChainLength()) {
        // DEC-034 flush. Set chainDepth on the new head immediately before
        // pushChain (Phase-4 caller invariant); the publishing RELEASE-CAS
        // propagates all prior chain-element writes (DEC-042 #1).
        m.head->chainDepth = m.depth;
        stack->pushChain(*m.head, m.depth);
        m.head  = nullptr;
        m.depth = 0;
    }
}

// ─── Phase 7 entry-point predicates ────────────────────────────────────────

// DEC-014 (amended ITEM-052, + #UD/#DF per user direction): vmsmalloc / vmsfree
// must not run from IRQ/NMI/#UD/#DF/#GP/#MC context (same-CPU reentry hazard on
// the magazine). #PF is conditionally legal and is NOT in the set. Reads the
// per-CPU interrupt-context depth counters maintained by the InterruptContextGuard.
inline bool inForbiddenContextForVmsmalloc() {
    const auto& d = kernel::interrupts::currentCpuInterruptDepths();
    return d.irq > 0 || d.nmi > 0 || d.ud > 0
        || d.df  > 0 || d.gp  > 0 || d.mc > 0;
}

// DEC-030 caller-side contract (P7-DEC-003): the caller must hold the thread
// non-preemptible and pinned to its current CPU. Two separate predicates name
// the two distinct obligations; both are vacuously true until CroCOS has a
// preemptive scheduler, at which point only these bodies change.
inline bool preemptionDisabled() {
    // TODO(DEC-030, future scheduler): per-CPU preempt-count check.
    return true;
}
inline bool cpuPinned() {
    // TODO(DEC-030, future scheduler): per-thread migrate-disable check.
    return true;
}

}  // namespace

// ─── Phase-3 init hooks (declared in VMSubstrateSlab.h) ────────────────────

void vmsmallocLateInit(uintptr_t base, size_t size) {
    vmsBase = base;
    vmsSize = size;
    // P5-DEC-003: the live VA window must fit the DEC-015 page-offset budget.
    assert(vmsSize <= (uintptr_t{1} << kVmsRegionVABits),
           "DEC-015: VMSubstrate VA window exceeds page-offset budget");

    // Construct a ChainedTreiberStack per (CPU-bearing domain, class), with
    // its Hooks bound to the matching tuning row. Storage was zero-filled by
    // reservePerDomainStaticBuffer; the constructor seeds maxChainLength =
    // kInitialK and leaves the head encoding the empty stack (zero).
    for (size_t d = 0; d < kMaxDomains; d++) {
        if (perDomainBufs[d] == nullptr) continue;  // not CPU-bearing
        const numa::DomainID dom{static_cast<uint16_t>(d)};
        PartialStackStorage* storage = partialStorageFor(dom);
        MagazineTuning*      tuning  = tuningFor(dom);
        for (size_t c = 0; c < kNumSizeClasses; c++) {
            new (&storage[c]) PartialStack(kInitialK, VmsmallocHooks{ &tuning[c] });
        }
    }
}

void vmsmallocBootSmoke() {
    const arch::ProcessorID i = arch::getCurrentProcessorID();
    const numa::DomainID localDomain = numa::numaPolicy().homeDomain(i);

    // ── Phase 6: same-domain becameAvailable publish + flush (class 7) ──
    //
    // Run first, while every magazine and shared stack is pristine. Class 7
    // (512 B) has the fewest slots per slab, so filling K slabs to Full is
    // cheap. Fill K = maxChainLength slabs (each becameFull pop empties the
    // magazine as we go), then free one slot per slab: each free drives a
    // Full→Partial transition (becameAvailable) that prepends the slab to the
    // magazine, and the K-th free trips the flush to the shared stack.
    {
        const size_t   c    = kNumSizeClasses - 1;        // 512 B
        const size_t   n    = slotCount(c);
        Magazine&      m    = kernel::cpuLocal().magazines[c];
        const uint32_t kmax = partialFor(localDomain, c)->getMaxChainLength();
        assert(kmax >= 2 && kmax <= kMaxK, "vmsmalloc smoke: unexpected maxChainLength");

        void* firstSlotOfSlab[kMaxK];
        for (uint32_t s = 0; s < kmax; s++) {
            for (size_t k = 0; k < n; k++) {
                void* p = VMSubstrate::vmsmalloc(slotSize(c));
                if (k == 0) {
                    firstSlotOfSlab[s] = p;
                    assert((reinterpret_cast<uintptr_t>(p) & (slotAlignment(c) - 1)) == 0,
                           "vmsmalloc smoke: slot misaligned, class 7");
                    SlabDescriptorBase* d = reinterpret_cast<SlabDescriptorBase*>(
                        reinterpret_cast<uintptr_t>(p) & ~(arch::smallPageSize - 1));
                    assert(d->magic == kSlabDescriptorMagic,
                           "vmsmalloc smoke: bad descriptor magic, class 7");
                }
            }
        }
        // Every filled slab became Full and was popped off the magazine.
        assert(m.depth == 0 && m.head == nullptr,
               "vmsmalloc smoke: magazine should be empty after filling slabs to Full");

        for (uint32_t s = 0; s < kmax; s++) {
            VMSubstrate::vmsfree(firstSlotOfSlab[s]);  // Full→Partial → publish
            if (s + 1 < kmax) {
                assert(m.depth == s + 1 && m.head == reinterpret_cast<SlabDescriptorBase*>(
                           reinterpret_cast<uintptr_t>(firstSlotOfSlab[s]) & ~(arch::smallPageSize - 1)),
                       "vmsmalloc smoke: becameAvailable publish did not extend the magazine");
            }
        }
        // The K-th publish tripped the flush: magazine empty, chain on the stack.
        assert(m.depth == 0 && m.head == nullptr,
               "vmsmalloc smoke: magazine should be empty after flush");
        assert(!partialFor(localDomain, c)->empty(),
               "vmsmalloc smoke: shared stack should hold the flushed chain");
        klog() << "vmsmalloc smoke: same-domain publish+flush OK (class "
               << static_cast<uint64_t>(c) << ", K=" << static_cast<uint64_t>(kmax) << ")\n";
    }

    // ── P6-DEC-006: fill-to-Full → free → realloc, per class (classes 0..6) ──
    //
    // P6-DEC-006's intent is the becameAvailable reuse path: a Full→Partial
    // transition publishes the slab back to the magazine, and the next alloc
    // hands back the freed slot. (The decision's "same slot" only holds via
    // this path — Phase 1's SplitBitmap parks a freed bit in the free half and
    // reclaims it only when the alloc half is exhausted, i.e. the slab was
    // Full; a naive single alloc/free/alloc would hand out the next sequential
    // slot instead.) So fill the slab to capacity, free one slot, and confirm
    // the realloc reuses exactly that slot.
    for (size_t c = 0; c < kNumSizeClasses - 1; c++) {
        const size_t n = slotCount(c);
        void* last = nullptr;
        for (size_t k = 0; k < n; k++) {
            last = VMSubstrate::vmsmalloc(slotSize(c));  // fill to Full
        }
        assert((reinterpret_cast<uintptr_t>(last) & (slotAlignment(c) - 1)) == 0,
               "vmsmalloc smoke: slot misaligned for class ", static_cast<uint64_t>(c));
        // The Full slab was popped off the magazine by the becameFull pop.
        Magazine& m = kernel::cpuLocal().magazines[c];
        assert(m.depth == 0 && m.head == nullptr,
               "vmsmalloc smoke: magazine should be empty after filling slab, class ",
               static_cast<uint64_t>(c));

        VMSubstrate::vmsfree(last);  // Full→Partial → becameAvailable → publish
        assert(m.depth == 1 && m.head != nullptr && m.head->magic == kSlabDescriptorMagic,
               "vmsmalloc smoke: becameAvailable did not publish to the magazine, class ",
               static_cast<uint64_t>(c));

        void* p2 = VMSubstrate::vmsmalloc(slotSize(c));  // drains free half → same slot
        assert(p2 == last,
               "vmsmalloc smoke: becameAvailable realloc did not reuse the freed slot, class ",
               static_cast<uint64_t>(c));
        klog() << "vmsmalloc smoke: class " << static_cast<uint64_t>(c)
               << " size " << static_cast<uint64_t>(slotSize(c))
               << " fill/free/reuse OK -> " << p2 << "\n";
    }

    // ── DEC-029 whole-page alloc/free cycle ──
    void* w = VMSubstrate::vmsmalloc(1024);  // above largest slab class (512)
    assert((reinterpret_cast<uintptr_t>(w) & (arch::smallPageSize - 1)) == 0,
           "vmsmalloc smoke: whole-page bypass not page-aligned");
    VMSubstrate::vmsfree(w);
    klog() << "vmsmalloc smoke: DEC-029 whole-page cycle OK " << w << "\n";
}

}  // namespace kernel::mm::vmsmalloc

// ─── Public entry point ────────────────────────────────────────────────────

namespace kernel::mm::VMSubstrate {

void* vmsmalloc(size_t size) {
    using namespace kernel::mm::vmsmalloc;

    // Entry-point contract asserts (Phase 7, all debug-only per P7-DEC-001).
    // Release builds trust the caller; bad inputs misbehave silently.
    assert(size > 0, "vmsmalloc: size must be positive");                   // DEC-023
    assert(size <= arch::smallPageSize, "vmsmalloc: size exceeds page");    // DEC-004
    assert(!inForbiddenContextForVmsmalloc(),                               // DEC-014
           "vmsmalloc: illegal in IRQ/NMI/#UD/#DF/#GP/#MC context");
    assert(preemptionDisabled(), "vmsmalloc: caller must hold preempt-disable");  // DEC-030
    assert(cpuPinned(), "vmsmalloc: caller must be pinned to current CPU");        // DEC-030

    // DEC-029 whole-page bypass. sizeClassFor returns the kNumSizeClasses
    // sentinel for sizes above the largest slab class. (The size<=pageSize
    // guard is now the debug-only entry assert above — DEC-004 hoist.)
    const size_t c = sizeClassFor(size);
    if (c >= kNumSizeClasses) {
        return allocPage();  // panics on failure (DEC-012)
    }

    const arch::ProcessorID i = arch::getCurrentProcessorID();
    const numa::DomainID localDomain = numa::numaPolicy().homeDomain(i);
    Magazine& m = kernel::cpuLocal().magazines[c];  // P7-DEC-010

    while (true) {
        if (m.depth > 0) {
            // Fast path — DEC-037 unified magazine + DEC-039 pre-read.
            SlabDescriptorBase* head = m.head;
            // DEC-039: pre-read m.head->chainNext BEFORE allocSlot. Do NOT
            // re-read head->chainNext after allocSlot — a same-domain freer's
            // push may overwrite it. The bookkeeper's ACQ_REL allocSlot
            // (DEC-042 #4) orders this pre-read ahead of any later freer write.
            SlabDescriptorBase* nextLocal = head->chainNext.load(RELAXED);

            Core::OccupancyTransition transition{};
            const int slot = dispatchOnClass(c, head, [&](auto* concrete) {
                return concrete->bookkeeper.allocSlot(transition);
            });
            // Magazine slabs are Partial or Empty (never Full) at fast-path
            // entry, so allocSlot always succeeds.
            assert(slot >= 0, "vmsmalloc: allocSlot failed on a non-full magazine head");

            if (transition.becameFull()) {
                m.head = nextLocal;
                m.depth--;
                if (m.depth > 0) {
                    // DEC-040 amended: lazy first-touch freshness on the new
                    // head before the next iteration reads its fields.
                    VMSubstrate::ensureTLBEntryFresh(m.head);
                }
            }
            return slotZeroAddr(head, c) + static_cast<size_t>(slot) * slotSize(c);
        }

        // m.depth == 0 — refill from the shared stack (DEC-019: no cross-domain
        // steal; a miss falls through to a fresh slab). Phase 4's pop fires the
        // VmsmallocHooks onPreTouch (ensureTLBEntryFresh) before reading the
        // popped head's fields, so popped.head is TLB-fresh on return.
        auto popped = partialFor(localDomain, c)->pop();  // P5-DEC-002
        if (popped.head != nullptr) {
            m.head  = popped.head;
            m.depth = popped.depth;

            // DEC-036 eager-free walk: drain Empty heads above the m.depth == 1
            // floor. isEmpty() reduces to "no live allocations" for vmsmalloc
            // slabs (reservedCount == kTailBits after seeding; P5-DEC-004). The
            // first head is fresh from the pop hook; each advance below pays a
            // lazy first-touch freshness on the new head (DEC-040 amended).
            while (m.depth > 1 &&
                   dispatchOnClass(c, m.head, [](auto* d) { return d->bookkeeper.isEmpty(); })) {
                SlabDescriptorBase* next = m.head->chainNext.load(RELAXED);
                VMSubstrate::freePage(m.head);
                m.head = next;
                m.depth--;
                if (m.depth > 0) {
                    VMSubstrate::ensureTLBEntryFresh(m.head);  // lazy first-touch
                }
            }
            continue;  // retry fast path
        }

        // Shared stack empty for (localDomain, c). The pop's onEmptyStack hook
        // already bumped starvationCount inside Phase 4 (P5-DEC-007). Build a
        // fresh slab on this CPU's home domain.
        m.head  = createFreshSlab(i, c);
        m.depth = 1;
        // No ensureTLBEntryFresh — allocPage already invalidated this CPU's
        // TLB entry for the page (DEC-046).
        continue;  // retry fast path
    }
}

void vmsfree(void* p) {
    using namespace kernel::mm::vmsmalloc;

    // Entry-point contract asserts (Phase 7, all debug-only per P7-DEC-001).
    assert(p != nullptr, "vmsfree: pointer must be non-null");              // DEC-023
    assert(!inForbiddenContextForVmsmalloc(),                              // DEC-014
           "vmsfree: illegal in IRQ/NMI/#UD/#DF/#GP/#MC context");
    assert(preemptionDisabled(), "vmsfree: caller must hold preempt-disable");  // DEC-030
    assert(cpuPinned(), "vmsfree: caller must be pinned to current CPU");        // DEC-030

    // Phase 6 carries the DEC-026 validation chain; each step depends on its
    // predecessors, so the source order is load-bearing (DEC-026 amended).
    const uintptr_t pa = reinterpret_cast<uintptr_t>(p);

    // DEC-026 range check — BEFORE the freshness call, so an arbitrary VA never
    // reaches ensureTLBEntryFresh.
    assert(pa >= vmsBase && pa < vmsBase + vmsSize,
           "vmsfree: pointer outside VMSubstrate range");

    // DEC-029 whole-page bypass — BEFORE the freshness call, so a bad but
    // page-aligned pointer in an unpopulated arena doesn't fault inside
    // ensureTLBEntryFresh. Whole-page frees read no descriptor.
    if ((pa & (arch::smallPageSize - 1)) == 0) {
        VMSubstrate::freePage(p);  // freePage asserts on its own validation
        return;
    }

    // DEC-016/DEC-026: freer-side first-touch freshness before any read of
    // *desc. Every subsequent descriptor read is through a fresh TLB mapping.
    VMSubstrate::ensureTLBEntryFresh(p);

    SlabDescriptorBase* desc = reinterpret_cast<SlabDescriptorBase*>(
        pa & ~(arch::smallPageSize - 1));

    // DEC-013/DEC-044 magic check — guards the slot-range arithmetic against
    // non-slab pages.
    assert(desc->magic == kSlabDescriptorMagic,
           "vmsfree: descriptor magic mismatch (not a slab page)");

    // ITEM-048 slot-range bounds. The lower bound rejects pointers inside the
    // descriptor region, which would otherwise pass the modulo via
    // two's-complement underflow; the upper bound rejects past-the-end.
    const uintptr_t slot0 = reinterpret_cast<uintptr_t>(slotZeroAddr(desc));
    const size_t    ss    = slotSize(desc);
    const size_t    nslot = slotCount(desc);
    assert(pa >= slot0, "vmsfree: pointer inside slab descriptor region");
    assert(pa < slot0 + ss * nslot, "vmsfree: pointer past slab data region");

    // DEC-013 modulo check.
    const uintptr_t off = pa - slot0;
    assert(off % ss == 0, "vmsfree: misaligned slot pointer");
    const size_t slotIdx = off / ss;

    // DEC-024 debug-only poison — BEFORE freeSlot, so the slot is still
    // allocated (from the bookkeeper's view) while we scribble it, with no
    // concurrent reallocation possible. 0xCC is the int3 trip-wire.
#ifdef DEBUG_BUILD
    memset(p, 0xCC, ss);
#endif

    // DEC-026 bookkeeper free (8-way dispatch on sizeClass — P5-DEC-001 sibling).
    Core::OccupancyTransition transition{};
    dispatchOnClass(desc->sizeClass, desc, [&](auto* concrete) {
        concrete->bookkeeper.freeSlot(slotIdx, transition);
        return 0;  // dispatchOnClass deduces a common return type across cases
    });

    // DEC-019/DEC-034 conditional publish on Full→Partial. P6-DEC-005: read
    // numaDomain only here, on the publishing branch.
    if (transition.becameAvailable()) {
        const numa::DomainID home = desc->numaDomain;  // DEC-018, set at creation
        const arch::ProcessorID i = arch::getCurrentProcessorID();
        const numa::DomainID localDomain = numa::numaPolicy().homeDomain(i);
        if (home == localDomain) {
            sameDomainPublishAndMaybeFlush(i, desc);   // DEC-034
        } else {
            crossDomainPublish(desc);                  // DEC-019 gate, P6-DEC-002
        }
    }
}

}  // namespace kernel::mm::VMSubstrate
