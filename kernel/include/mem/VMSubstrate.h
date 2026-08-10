//
// Created by Spencer Martin on 4/26/26.
//

#ifndef CROCOS_VMSUBSTRATE_H
#define CROCOS_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <kmemlayout.h>   // pageTableLevelForKMemRegion (for kWindowAddressBits)
// P7-DEC-007: make<T>'s compile-time alignment/size checks need the size-class
// accessors (sizeClassFor / slotAlignment / kNumSizeClasses). VMSubstrateSlab.h
// is the implementation-internal vmsmalloc header; every includer of this
// public header is a kernel TU with kernel/mm on its include path.
#include <VMSubstrateSlab.h>

namespace kernel::mm::VMSubstrate {
    bool init();

    // Width, in bits, of the VMSubstrate VA window — one entry at the top of
    // its page-table subtree (PML4[VMM_SUBSTRATE_ROOT_INDEX] on AMD64 = 39
    // bits / 512 GiB). This is the substrate's own geometry; vmsmalloc's
    // DEC-015 packed-tagged-head encoding consumes it to size the page-offset
    // field (kOffsetBits = kWindowAddressBits - log2(smallPageSize)) instead
    // of reaching into arch::pageTableDescriptor directly. The userspace test
    // harness supplies the same constant from its MockVMSubstrate.
    inline constexpr unsigned kWindowAddressBits =
        arch::pageTableDescriptor.getVirtualAddressBitCount(
            pageTableLevelForKMemRegion() - 1);

    // Returns the base virtual address of the arena at the given index.
    virt_addr arenaVirtualBase(size_t index);
    void* allocPage();

    // vmsmalloc DEC-048: the failable sibling of allocPage. Returns null,
    // having reserved nothing, on arena-VA exhaustion, on failure to back the
    // lazily-installed page-table/occupancy pages a fresh VA needs, and on
    // physical-page exhaustion. `freePage` is the release for both.
    //
    // Exists because RadixVM reaches this path from mmap/fork on behalf of
    // untrusted userspace, which must get ENOMEM rather than a kernel panic.
    void* tryAllocPage();
    void freePage(void*);

    // DEC-047: slab-reclaim sibling of freePage. Used only by vmsmalloc's
    // DEC-036 eager-free walk. Behaves like freePage (real frame returned to the
    // allocator, VA released) but leaves the freed VA mapped read-only onto a
    // shared sentinel page, so a concurrent lock-free Treiber pop mid-flight on
    // the reclaimed slab descriptor reads harmless garbage instead of faulting
    // on a torn-down PTE. Whole-page frees keep using freePage (never Treiber
    // nodes). The userspace mock recycles like freePage (no page tables).
    void reclaimSlabPage(void*);

    // Returns whether it actually had to invalidate — i.e. whether this CPU's
    // mapping for that page WAS stale. Callers may ignore it; SafePtr does.
    // It exists so an instrumented consumer can distinguish "the freshness call
    // ran" from "the freshness call saved us", which is the difference between
    // exercising the DEC-047 hazard and merely walking past it (P4-ITEM-001).
    bool ensureTLBEntryFresh(void*);

#ifdef CROCOS_FRESHNESS_STATS
    // Count of reclaimSlabPage calls since boot (P4-DEC-006). Declared only
    // under the flag so a stats-less build cannot silently report 0 and be
    // misread as "the reclaim path never ran".
    uint64_t reclaimedSlabPageCount();
#endif

    // Convention-internal: external callers should prefer make<T> / destroy<T>.
    // These are unavoidably declared here because make<T> / destroy<T> are
    // templates whose bodies must live in this public header; structural hiding
    // is not possible under the template-visibility constraint. See parent-spec
    // DEC-028 (amended).
    void* vmsmalloc(size_t size);
    // DEC-048: the identical allocation path with null returns in place of the
    // exhaustion panics — both of them, the slab-creation slow path and the
    // DEC-029 whole-page bypass. `vmsfree` is the release for both.
    void* vmsmallocTry(size_t size);
    void vmsfree(void*);

    // Maps a physical MMIO page into the current CPU's arena with cache-disable semantics.
    // paddr must be page-aligned.  The returned virtual address is permanently mapped.
    void* mapMMIOPage(phys_addr paddr);

    // ─── vmsmalloc Phase 3 — init-only NUMA-aware pinned-buffer primitive ───
    //
    // Allocates `divideAndRoundUp(byteSize, smallPageSize)` physical pages
    // on NUMA domain `d`, maps them contiguously inside VMSubstrate's
    // static-buffer region, zero-fills, and returns the base VA. The
    // returned VA is page-aligned and pinned for the kernel's lifetime
    // (no freeing API). Single-threaded init context only — asserts in
    // debug builds; panics on PageAllocator failure or region exhaustion.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);

    // DEC-050 / DEC-051: the failable, runtime-callable variant.
    //
    // The panicking form above stays init-only. This one is additionally legal
    // at runtime **under the RCU domain-management lock**
    // (kernel::rcu::DomainManagementLockGuard), and returns null rather than
    // panicking on physical-page or window exhaustion. Both matter: the runtime
    // callers are Domain::init() and the radix per-address-space control block,
    // both reached from address-space creation, so untrusted userspace can drive
    // this path and must not be able to panic the kernel — it gets ENOMEM.
    //
    // Reservations are kernel-lifetime and are never returned to VMSubstrate;
    // callers recycle them through their own freelists, which is what makes the
    // reservation high-water-mark at the maximum concurrent address spaces
    // rather than grow with cumulative process churn.
    //
    // Zero-fill is a per-RESERVATION guarantee. A recycled block is the caller's
    // to re-zero.
    //
    // ─── PLANNED: a per-NUMA-domain pinned allocator should replace this ────
    //
    // This is a bump pointer over one 1 GiB arena slot with **no free path and
    // page granularity**, and both halves of that now cost real memory:
    //
    //   - **Page granularity.** The bump pointer advances by whole pages, so a
    //     768-byte reservation consumes 4 KiB. Measured on an 8-CPU machine: the
    //     radix control block wants 768 B and takes a page (81% wasted); RCU's
    //     reader slots want 1,024 B and take a page (75% wasted). Two consumers,
    //     two pages, ~1.8 KiB of 8 KiB actually used. Sizing either request
    //     smaller saves nothing — the waste is here, not in the callers.
    //
    //   - **No free path.** Every consumer that needs reuse hand-rolls a
    //     freelist over blocks this hands out: `kernel::rcu`'s
    //     `gFreeSlotBlocks` and radix's `ControlBlockFreelist` were the same
    //     structure written twice. The radix one had to grow NUMA partitioning
    //     and a size check (radix D-045) that the RCU one does not need only
    //     because its placement happens to be topology-derived rather than
    //     caller-supplied. A third consumer will write it a third time and get a
    //     different subset right.
    //
    // Both bullets are now addressed by `mm::PinnedBlockPool`, a layer over this
    // one — see its header. Radix reserves through it exclusively; RCU does for
    // slot arrays that fit a page (every machine up to 32 CPUs) and keeps
    // `gFreeSlotBlocks` for the multi-page case, which needs per-page placement
    // and the contiguity property noted below. What is written here stays true
    // of THIS entry point, which is what a new consumer reaching for it directly
    // needs to know.
    //
    // What is wanted: a pinned allocator **partitioned per NUMA domain**, with
    // sub-page suballocation and real free/reuse. Note what it does NOT need to
    // do — and this is what makes it tractable rather than a shootdown problem:
    // it must never unmap a page. DEC-051b's safety argument is that every entry
    // here "transitions not-present -> present EXACTLY ONCE and never changes",
    // which is precisely why nothing pinned carries an `ensureTLBEntryFresh`
    // obligation — and, since R-13, why nothing pinned may TAKE one: the slot has
    // no dirty bitmap, so the call's dirty-word arithmetic addresses live pinned
    // data and its `fetch_and` corrupts it. `ensureTLBEntryFresh` asserts against
    // pinned addresses; do not wrap pinned storage in a `SafePtr`.
    // Reusing a BLOCK inside an already-mapped page does not touch a
    // PTE at all: the VA -> physical mapping is unchanged and only the bytes
    // differ, so a CPU's cached translation stays correct. Free-and-reuse is
    // free; only returning pages to the PageAllocator would break the invariant,
    // and no consumer has asked for that.
    //
    // ─── The ceiling, and why it is not stated here any more (R-16) ─────────
    //
    // For scale: against the 1 GiB region, **~180,000 concurrent address spaces
    // on an 8-CPU machine**, at 5,939 B of window each.
    //
    // That figure is DERIVED, by
    // `radix_static_buffer_ceiling_is_derived_from_the_live_pools`
    // (`tests/kernel/radix/AddressSpaceTest.cpp`), from the live pools' own
    // strides — and it is derived there rather than asserted here because **this
    // sentence has been wrong twice.** It said ~131,000, then ~580,000 once
    // neither consumer paid a page minimum (radix D-047/D-048); D-051 then moved
    // the root bucket page into its own pool and D-059 took the control block's
    // stride from 832 B to 768 B, and each time the code that changed was three
    // files away from the prose describing it. A referee's re-derivation to
    // ~174,000 was also wrong, for the reason in the next paragraph.
    //
    // Two things to know if you need to move it. **The metric is each consumer's
    // SHARE OF A PAGE, not its stride** — a 768 B block costs 4,096/5 = 819 B of
    // window, because the sixth does not fit. And **the root bucket page
    // dominates**: it is a whole page against everything else's fractions (819 B
    // control block + 4,096 B root page + 1,024 B RCU slot array), so it is the
    // only row worth attacking. The test asserts that dominance, so the
    // conclusion cannot go stale either.
    //
    // The number is CPU-count dependent — the control block carries a per-CPU
    // pool array and the RCU slot array leaves the pooled path above 32 CPUs — so
    // read it as the consumer-desktop figure it is.
    void* tryReservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);

    // Returns the base VA of CPU `i`'s per-CPU CpuLocal page (sized via
    // kernel::kCpuLocalBytes in cpu_local.h). The page is allocated and
    // mapped during createArena(i) on the arena owner's NUMA domain, and
    // is zero-filled at that time. Pure address arithmetic — safe to call
    // from any context after VMSubstrate::init returns.
    void* cpuLocalPageFor(arch::ProcessorID i);

#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 3
    // D-052 mode 3 only: total `ensureTLBEntryFresh` calls since boot. Exists so
    // an operation's freshness-call COUNT can be measured on target, where the
    // descent cache makes it differ from the harness's structural per-level
    // figure. Compiled out of every other build.
    uint64_t freshnessCallCount();
#endif

    // ─── SafePtr ───────────────────────────────────────────────────────────
    //
    // A pointer into VMSubstrate-backed memory that discharges its freshness
    // obligation on every access. RCU says the object has not been recycled;
    // `ensureTLBEntryFresh` says THIS CPU's view of its bytes is current. The two
    // are orthogonal, and a pointer that carries only the first is the DEC-047
    // bug class waiting to happen — silently, since the userspace harness's
    // `ensureTLBEntryFresh` is a no-op and every test passes without it.
    //
    // **Per ACCESS, not per pointer, and that is the whole design.** A single
    // call at acquisition is not equivalent: freshness is a property of one
    // CPU's mapping, so a reference that outlives a section and crosses a
    // blocking round-trip (radix DEC-015's lookup result is exactly this) can
    // resume on a CPU that never made the call. Every `operator*`, `operator->`
    // and `at<U>` therefore pays it, and the ones that deliberately do not —
    // `address()`, `raw()`, the comparisons — say so in their names.
    //
    // The API grows with its consumer on purpose. The VMM is the only consumer,
    // so there is nothing to calcify around: when a site cannot be expressed
    // ergonomically, the answer is to widen this rather than to reach past it
    // for a bare `ensureTLBEntryFresh`, which is how the radix tree ended up
    // with five unguarded sites that an in-kernel stress had to find.

    template <typename T>
    struct SafePtr {
    private:
        T* ptr;
    public:
        SafePtr() : ptr(nullptr) {}
        SafePtr(T* p) : ptr(p) {}
        SafePtr(const SafePtr& other) = default;
        SafePtr(SafePtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        SafePtr<T>& operator=(SafePtr<T>&& other) noexcept {
            ptr = other.ptr; other.ptr = nullptr; return *this;
        }

        T& operator*() const { ensureTLBEntryFresh(ptr); return *ptr; }
        T* operator->() const { ensureTLBEntryFresh(ptr); return ptr; }

        // A sub-object at a byte offset, made fresh on ITS OWN page rather than
        // on the base's — which matters for anything that can straddle a page
        // boundary, and costs nothing when it cannot.
        template <typename U>
        U& at(size_t byteOffset) const {
            auto* const p = reinterpret_cast<unsigned char*>(ptr) + byteOffset;
            ensureTLBEntryFresh(p);
            return *reinterpret_cast<U*>(p);
        }

        // ── The deliberate non-accesses ────────────────────────────────────
        //
        // Comparison, encoding into a slot word, handing to `retire`, and
        // computing an address discharge NOTHING, because they read no bytes.
        // Named apart from the accessors so that "this one genuinely does not
        // need it" is sayable in the vocabulary rather than by omission.
        [[nodiscard]] T* address() const { return ptr; }
        [[nodiscard]] void* raw() const { return ptr; }

        // Hidden friends so a raw pointer on EITHER side works without the
        // caller reaching for `address()`. Comparison reads no bytes, so this
        // costs the discipline nothing — and an identity check that had to be
        // spelled `a.address() == b` would push people towards holding raw
        // pointers, which is the habit this type exists to break.
        friend bool operator==(const SafePtr& a, const SafePtr& b) { return a.ptr == b.ptr; }
        friend bool operator!=(const SafePtr& a, const SafePtr& b) { return a.ptr != b.ptr; }
        explicit operator bool() const { return ptr != nullptr; }
    };

    // The type-erased form, for a pointee whose type is decided per access
    // rather than by the pointer. The radix tree's `NodeRef` is the motivating
    // consumer and the reason this exists: a descent stands on nodes of mixed
    // valence and cannot name a concrete `Node<G, V>` (DEC-062's level->type map
    // is what avoids needing to), so `SafePtr<Node<...>>` cannot be formed —
    // yet the freshness obligation is identical. Without this the only way to
    // write that code is a bare `ensureTLBEntryFresh` plus a `reinterpret_cast`,
    // which is precisely the unguarded shape this type exists to prevent.
    template <>
    struct SafePtr<void> {
    private:
        void* ptr;
    public:
        SafePtr() : ptr(nullptr) {}
        SafePtr(void* p) : ptr(p) {}
        SafePtr(const SafePtr& other) = default;
        SafePtr<void>& operator=(const SafePtr<void>& other) = default;

        // The whole surface: a typed reference to a sub-object, fresh.
        template <typename U>
        U& at(size_t byteOffset) const {
            auto* const p = static_cast<unsigned char*>(ptr) + byteOffset;
            ensureTLBEntryFresh(p);
            return *reinterpret_cast<U*>(p);
        }

        [[nodiscard]] void* address() const { return ptr; }
        [[nodiscard]] void* raw() const { return ptr; }

        bool operator==(const SafePtr& other) const { return ptr == other.ptr; }
        bool operator!=(const SafePtr& other) const { return ptr != other.ptr; }
        explicit operator bool() const { return ptr != nullptr; }
    };

    namespace detail {
        // The size/alignment checks make<T> and tryMake<T> share. Spelled once
        // so the two entry points cannot drift.
        template <typename T>
        constexpr void checkMakeConstraints() {
            // DEC-004/DEC-029: T must fit in a page (slab class or whole-page bypass).
            constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
            static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                          "VMSubstrate::make<T>: sizeof(T) exceeds a page");
            // DEC-025: for slab-backed T, alignof(T) must not exceed its size class's
            // slot alignment. Short-circuits for the whole-page bypass (c ==
            // kNumSizeClasses), whose pageSize alignment dominates any alignof(T) —
            // the `c >= kNumSizeClasses` term also keeps slotAlignment(c) from being
            // evaluated out of range in that case.
            static_assert(c >= vmsmalloc::kNumSizeClasses ||
                              alignof(T) <= vmsmalloc::slotAlignment(c),
                          "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                          "size class. Pad T into a power-of-two size class, split the "
                          "over-aligned subobject, or use a different allocator.");
        }

        // malloc (not calloc) semantics, per DEC-024 ("does not zero-initialize
        // returned memory in any build configuration"): with no constructor
        // arguments, *default*-initialize — a trivially-constructible T is left
        // UNINITIALIZED rather than zero-filled. (Value-init `T()` would memset
        // the storage, which dominated allocator profiles.) Callers needing
        // zeroed storage construct it explicitly (e.g. make<T>(T{}) or a T whose
        // constructor zeroes).
        template <typename T, typename... Ts>
        SafePtr<T> constructInto(void* mem, Ts&&... args) {
            if constexpr (sizeof...(Ts) == 0) {
                return new (mem) T;                       // default-initialization
            } else {
                return new (mem) T(forward<Ts>(args)...); // direct-initialization
            }
        }
    }

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        detail::checkMakeConstraints<T>();
        return detail::constructInto<T>(vmsmalloc(sizeof(T)), forward<Ts>(args)...);
    }

    // DEC-048 / radix DEC-075. RadixVM allocates exclusively through this; every
    // call site handles null. §10's inverted hazard is that a site written
    // against never-null make<T> re-imports the panic through the back door,
    // which is why the tree must not name `make` at all.
    template <typename T, typename... Ts>
    SafePtr<T> tryMake(Ts&&... args) {
        detail::checkMakeConstraints<T>();
        void* mem = vmsmallocTry(sizeof(T));
        if (!mem) return SafePtr<T>(nullptr);
        return detail::constructInto<T>(mem, forward<Ts>(args)...);
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) {
            obj->~T();
            vmsfree(obj.raw());
        }
    }
}

#endif //CROCOS_VMSUBSTRATE_H
