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
#include <core/atomic.h>
#include <core/atomic/TreiberStack.h>
#include <core/math.h>
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

    // One allocation per slab-backed class: alignment (DEC-001), magic, the
    // magazine head becoming non-null, and the starvation counter advancing
    // exactly once (the shared stack starts empty, so the first allocation per
    // class takes the fresh-slab slow path).
    for (size_t c = 0; c < kNumSizeClasses; c++) {
        const uint32_t starvBefore =
            tuningFor(localDomain)[c].starvationCount.load(RELAXED);

        void* p = VMSubstrate::vmsmalloc(slotSize(c));
        const uintptr_t pa = reinterpret_cast<uintptr_t>(p);

        assert((pa & (slotAlignment(c) - 1)) == 0,
               "vmsmalloc smoke: slot misaligned for class ", static_cast<uint64_t>(c));
        Magazine& m = kernel::cpuLocal().magazines[c];
        assert(m.head != nullptr,
               "vmsmalloc smoke: magazine head null after first alloc, class ",
               static_cast<uint64_t>(c));
        assert(m.head->magic == kSlabDescriptorMagic,
               "vmsmalloc smoke: bad descriptor magic, class ", static_cast<uint64_t>(c));
        const uint32_t starvAfter =
            tuningFor(localDomain)[c].starvationCount.load(RELAXED);
        assert(starvAfter == starvBefore + 1,
               "vmsmalloc smoke: starvationCount must advance once on first alloc, class ",
               static_cast<uint64_t>(c));

        klog() << "vmsmalloc smoke: class " << static_cast<uint64_t>(c)
               << " size " << static_cast<uint64_t>(slotSize(c))
               << " -> " << p << "\n";
    }

    // Two successive class-0 (8 B) allocations land consecutive slots in the
    // same slab (in-magazine reuse): the second is exactly slotSize bytes past
    // the first.
    void* a = VMSubstrate::vmsmalloc(8);
    void* b = VMSubstrate::vmsmalloc(8);
    assert(reinterpret_cast<uintptr_t>(b) == reinterpret_cast<uintptr_t>(a) + 8,
           "vmsmalloc smoke: consecutive 8 B allocations not adjacent");
    klog() << "vmsmalloc smoke: class 0 reuse " << a << " then " << b << "\n";

    // Whole-page bypass classes (DEC-029): page-aligned, no slab descriptor.
    // A plain array (not a braced-init list) avoids std::initializer_list,
    // which the freestanding build does not provide.
    const size_t bypassSizes[] = { size_t{513}, size_t{1024}, size_t{2048}, size_t{4096} };
    for (size_t size : bypassSizes) {
        void* p = VMSubstrate::vmsmalloc(size);
        assert((reinterpret_cast<uintptr_t>(p) & (arch::smallPageSize - 1)) == 0,
               "vmsmalloc smoke: whole-page bypass not page-aligned");
        klog() << "vmsmalloc smoke: bypass size " << static_cast<uint64_t>(size)
               << " -> " << p << "\n";
    }
}

}  // namespace kernel::mm::vmsmalloc

// ─── Public entry point ────────────────────────────────────────────────────

namespace kernel::mm::VMSubstrate {

void* vmsmalloc(size_t size) {
    using namespace kernel::mm::vmsmalloc;

    // DEC-029 whole-page bypass. sizeClassFor returns the kNumSizeClasses
    // sentinel for sizes above the largest slab class.
    const size_t c = sizeClassFor(size);
    if (c >= kNumSizeClasses) {
        // Load-bearing branch guard: without it vmsmalloc(8000) would silently
        // hand back a 4 KiB page. Phase 7 hoists this to the entry-point chain.
        assert(size <= arch::smallPageSize, "vmsmalloc: size exceeds page");
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

}  // namespace kernel::mm::VMSubstrate
