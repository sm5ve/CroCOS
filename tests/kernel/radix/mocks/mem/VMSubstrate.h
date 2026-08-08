//
// radix-tree Phase 0 — mock <mem/VMSubstrate.h> carrying the DEC-052 UAGP
// oracle.
//
// Sibling of tests/kernel/vmsmalloc/mocks/mem/VMSubstrate.h: same page
// primitives, same mmap-backed MockVMSubstrate.cpp underneath (the radix
// harness compiles that file directly), same kWindowAddressBits. What this one
// adds is the instrument Phase 0 exists to build:
//
//   1. **The UAGP oracle** (DEC-052). Poison on destroy<T>, unpoison on
//      make<T>/tryMake<T> — attached to the refcount-zero destroy, NOT to the
//      RCU deleter. That placement is the decision, not an accident: under
//      DEC-035 the deleter *releases a reference*, it does not free, and it
//      legitimately touches the object (slot reads, Mapping releases, pool
//      pushes). Poisoning there false-positives on every legal deleter access,
//      and an instrument that cries wolf gets deleted — which is how the
//      project would lose the only detector it has for a class of defect that
//      fails as a silent wrong answer.
//
//   2. **Live-object accounting**, per type. LSan is structurally blind here:
//      the arena is one live mmap region, so a node leaked inside it has no
//      distinguishable footprint (§10). Explicit counters are the only leak
//      detector this harness can have.
//
//   3. **Allocation fault injection** — a scriptable null from tryMake. §9's
//      allocation-failure rows and DEC-101's creation unwind are otherwise
//      untestable; cheap to build now, needed from Phase 3.
//
// NOTE the poison ordering in destroy<T>: destructor, then vmsfree, THEN
// poison. Poisoning before vmsfree would have the allocator's own validation
// and (in DEBUG_BUILD) its DEC-024 0xCC scribble run over poisoned memory —
// an ASan report against vmsmalloc for doing its job. The window between
// vmsfree and the poison is not a hole: nothing may touch a freed slot, and
// the next make<T> on that slot unpoisons before constructing.
//

#ifndef CROCOS_MOCK_RADIX_VMSUBSTRATE_H
#define CROCOS_MOCK_RADIX_VMSUBSTRATE_H

#include <stddef.h>
#include <stdint.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <core/utility.h>      // forward, placement new
#include <VMSubstrateSlab.h>   // sizeClassFor / slotAlignment / kNumSizeClasses

#include <asan_poison.h>       // tests/ is on the include path

namespace kernel::mm::VMSubstrate {

    inline constexpr unsigned kWindowAddressBits = 39;

    // ─── Page primitives (MockVMSubstrate.cpp, shared with the vmsmalloc harness) ───
    void* allocPage();
    void  freePage(void* p);
    void  reclaimSlabPage(void* p);
    void* mapMMIOPage(phys_addr paddr);

    inline bool ensureTLBEntryFresh(void*) noexcept { return false; }  // userspace: never stale

    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);
    virt_addr arenaVirtualBase(size_t index);

    void* vmsmalloc(size_t size);
    void  vmsfree(void*);

    // ─── The oracle's out-of-line half ─────────────────────────────────────
    namespace oracle {
        // Stable small integer per type, assigned on first use. Registration is
        // mutex-guarded and the per-type id is a magic static, so this is safe
        // to reach from the harness's CPU threads.
        size_t registerType(const char* name, size_t bytes);
        void   noteConstructed(size_t typeId);
        void   noteDestroyed(size_t typeId);

        // Consulted by tryMake before it allocates. Returns true when this call
        // should report exhaustion. Never consulted by make (the panicking
        // contract has no failure to inject).
        bool   shouldInjectFailure();

        template <typename T>
        inline size_t typeIdOf() {
            static const size_t id = registerType(__PRETTY_FUNCTION__, sizeof(T));
            return id;
        }
    }

    template <typename T>
    struct SafePtr {
    private:
        T* ptr;
    public:
        SafePtr(T* p) : ptr(p) {}
        SafePtr(const SafePtr& other) : ptr(other.ptr) {}
        SafePtr(SafePtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
        T& operator*() const { ensureTLBEntryFresh(ptr); return *ptr; }
        T* operator->() const { ensureTLBEntryFresh(ptr); return ptr; }
        bool operator==(const SafePtr& other) const { return ptr == other.ptr; }
        bool operator!=(const SafePtr& other) const { return ptr != other.ptr; }
        operator bool() const { return ptr != nullptr; }
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        void* raw() const { return ptr; }
    };

    namespace detail {
        // The size/alignment checks make<T> and tryMake<T> share (DEC-004 /
        // DEC-025). Spelled once so the two entry points cannot drift.
        template <typename T>
        constexpr void checkMakeConstraints() {
            constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
            static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                          "VMSubstrate::make<T>: sizeof(T) exceeds a page");
            static_assert(c >= vmsmalloc::kNumSizeClasses ||
                              alignof(T) <= vmsmalloc::slotAlignment(c),
                          "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                          "size class. Pad T into a power-of-two size class, split the "
                          "over-aligned subobject, or use a different allocator.");
        }

        // Unpoison-then-construct-then-account. Shared by make and tryMake so
        // the oracle cannot be wired into one and forgotten on the other —
        // which would show up as a phantom leak, not as a missing poison, and
        // would be blamed on the tree.
        template <typename T, typename... Ts>
        SafePtr<T> constructInto(void* mem, Ts&&... args) {
            CroCOSTest::unpoisonRegion(mem, sizeof(T));
            oracle::noteConstructed(oracle::typeIdOf<T>());
            if constexpr (sizeof...(Ts) == 0) {
                return new (mem) T;                        // default-init, malloc semantics
            } else {
                return new (mem) T(forward<Ts>(args)...);
            }
        }
    }

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        detail::checkMakeConstraints<T>();
        return detail::constructInto<T>(vmsmalloc(sizeof(T)), forward<Ts>(args)...);
    }

    // DEC-048 / DEC-075. The tree allocates exclusively through this; every
    // call site handles null. §10's inverted hazard is that a site written
    // against never-null make<T> re-imports the panic through the back door,
    // which is why the tree must not name `make` at all.
    template <typename T, typename... Ts>
    SafePtr<T> tryMake(Ts&&... args) {
        detail::checkMakeConstraints<T>();
        if (oracle::shouldInjectFailure()) return SafePtr<T>(nullptr);
        void* mem = vmsmalloc(sizeof(T));
        if (!mem) return SafePtr<T>(nullptr);
        return detail::constructInto<T>(mem, forward<Ts>(args)...);
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (!obj) return;
        void* raw = obj.raw();
        obj->~T();
        oracle::noteDestroyed(oracle::typeIdOf<T>());
        vmsfree(raw);
        // AFTER the free — see the header comment. This is the store that turns
        // every use-after-grace-period in this harness into an ASan report.
        CroCOSTest::poisonRegion(raw, sizeof(T));
    }

    // ─── Harness control surface (not in the real header) ───
    namespace test {
        void   initialize(size_t cpuCount, size_t domainCount);
        void   shutdown();
        size_t activePageCount();
    }
}

#endif // CROCOS_MOCK_RADIX_VMSUBSTRATE_H
