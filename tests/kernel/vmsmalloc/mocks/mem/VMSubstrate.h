//
// vmsmalloc Phase 8 — mock <mem/VMSubstrate.h> for the userspace harness.
//
// Shadows the real kernel header via include-path ordering. Declares the same
// VMSubstrate API surface vmsmalloc.cpp links against, but the page-allocation
// primitives are backed by an mmap region (see MockVMSubstrate.cpp) rather than
// kernel page tables. ensureTLBEntryFresh is a no-op (no TLB in userspace).
//
// kWindowAddressBits is the literal 39 (one AMD64 PML4 entry / 512 GiB), so
// vmsmalloc's DEC-015 encoding uses the same 27-bit offset / 37-bit counter
// split as production (P8-DEC-007). The mmap region is far smaller, so the
// upper offset bits stay zero — identical math, no test-only code path.
//
// make<T> / destroy<T> / SafePtr mirror the real header so the (deferred)
// MakeStaticAssertsTest can exercise them; the integration tests call
// vmsmalloc / vmsfree directly.
//

#ifndef CROCOS_MOCK_VMSUBSTRATE_H
#define CROCOS_MOCK_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <core/utility.h>      // forward, placement new
#include <VMSubstrateSlab.h>   // sizeClassFor / slotAlignment / kNumSizeClasses

namespace kernel::mm::VMSubstrate {

    // VA-window width: literal 39 bits in the harness (P8-DEC-007). vmsmalloc's
    // VmsHeadEncoding derives kOffsetBits = 39 - log2(pageSize) = 27 from this.
    inline constexpr unsigned kWindowAddressBits = 39;

    // Page primitives — mmap-backed (MockVMSubstrate.cpp).
    void* allocPage();
    // DEC-048: the failable sibling. Null on pool exhaustion or under the
    // harness's scripted-failure hook (test::setPageAllocFailAt).
    void* tryAllocPage();
    void  freePage(void* p);
    // DEC-047 slab-reclaim sibling of freePage. The harness does not simulate
    // the read-only sentinel remap (no page tables in userspace); it recycles
    // the page like freePage. See MockVMSubstrate.cpp.
    void  reclaimSlabPage(void* p);
    void* mapMMIOPage(phys_addr paddr);

    // No-op in userspace: no TLB, no dirty bitmap. The call site in
    // vmsmalloc.cpp is still exercised (source-order correctness).
    inline bool ensureTLBEntryFresh(void*) noexcept { return false; }  // userspace: never stale

    // Phase-3 storage primitives.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    // vmsmalloc DEC-050/051: the failable, runtime-callable variant. The mock
    // fails on window exhaustion and under the harness's scripted-failure hook,
    // so the RCU domain-lifecycle unwind paths are drivable here.
    void* tryReservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);
    virt_addr arenaVirtualBase(size_t index);

    // Convention-internal (defined by vmsmalloc.cpp, compiled into the harness).
    void* vmsmalloc(size_t size);
    // DEC-048: same path, null returns in place of the two exhaustion panics.
    void* vmsmallocTry(size_t size);
    void  vmsfree(void*);

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

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::make<T>: sizeof(T) exceeds a page");
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                      "size class. Pad T into a power-of-two size class, split the "
                      "over-aligned subobject, or use a different allocator.");
        auto* mem = vmsmalloc(sizeof(T));
        // malloc (not calloc) semantics — mirror the real header: default-init
        // with no args so trivial T is left uninitialized, not zero-filled.
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;
        } else {
            return new (mem) T(forward<Ts>(args)...);
        }
    }

    // DEC-048's failable entry point, mirroring the real header.
    template <typename T, typename... Ts>
    SafePtr<T> tryMake(Ts&&... args) {
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::tryMake<T>: sizeof(T) exceeds a page");
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::tryMake<T>: alignof(T) exceeds the slot alignment of its "
                      "size class.");
        void* mem = vmsmallocTry(sizeof(T));
        if (!mem) return SafePtr<T>(nullptr);
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;
        } else {
            return new (mem) T(forward<Ts>(args)...);
        }
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) { obj->~T(); vmsfree(obj.raw()); }
    }

    // ─── Harness control surface (not in the real header) ───
    namespace test {
        // mmap the VA region, carve per-domain + per-CPU storage, construct the
        // per-(domain,class) ChainedTreiberStacks (via vmsmallocLateInit).
        void initialize(size_t cpuCount, size_t domainCount);
        void shutdown();              // munmap + reset all state
        size_t activePageCount();     // live slab/whole-page pages (leak check)
        // Scripted failure for tryReservePerDomainStaticBuffer: fail every call
        // from the n'th onward (-1 disables). Lets a consumer's creation-unwind
        // path be driven without first exhausting a 64 MiB arena.
        void   setStaticReservationFailAt(long n);
        // Same idea for tryAllocPage (DEC-048): fail every call from the n'th
        // onward (-1 disables). Drives vmsmallocTry's two exhaustion sites.
        void   setPageAllocFailAt(long n);
    }
}

#endif // CROCOS_MOCK_VMSUBSTRATE_H
