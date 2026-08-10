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
    // DEC-048: the failable sibling. Null on pool exhaustion or under
    // test::setPageAllocFailAt.
    void* tryAllocPage();
    void  freePage(void* p);
    void  reclaimSlabPage(void* p);
    void* mapMMIOPage(phys_addr paddr);

    // ─── Freshness accounting (D-049) ──────────────────────────────────────
    //
    // Userspace has no TLB, so this can only ever answer "not stale". What it
    // CAN do is record that it was called — and that is the difference between
    // a harness that finds this bug class and one that cannot.
    //
    // Every member of the class so far (§1.1's six sites, D-049's two) was
    // found by an in-kernel stress boot, one per boot, because a no-op that
    // records nothing makes "did this path discharge its obligation?"
    // unaskable: the tests pass identically with every call present or absent.
    // With a record, it becomes an ordinary assertion.
    //
    // **Per thread, and armed explicitly.** Per thread because freshness IS a
    // per-CPU property — "this thread discharged freshness for this page" is
    // the exact proposition, and a global counter would answer a weaker one
    // while adding cross-thread contention to every SafePtr access in the
    // torture suite. Armed explicitly because disarmed it costs one
    // thread-local bool test, so the concurrent and soak runs are unperturbed.
    namespace test {
        extern thread_local bool freshnessRecordingArmed;
        void recordFreshnessCall(const void* p);
        // R-13: pinned storage has no dirty bitmap, so the kernel's dirty-word
        // arithmetic would land in live pinned data. Mirrored here so the mistake
        // is catchable by a unit test rather than only by a debug kernel boot.
        bool isPinnedStaticBufferAddress(const void* p);
    }

    // NOT `noexcept`, deliberately, and it matches the real declaration
    // (`kernel/include/mem/VMSubstrate.h`) which carries none either. The mock's
    // copy had acquired one, and with the R-13 guard below that turns a catchable
    // assertion into `std::terminate` — a negative test cannot exercise a refusal
    // that kills the runner.
    inline bool ensureTLBEntryFresh(void* p) {
        // The kernel asserts this and would CORRUPT without the assert (R-13): a
        // pinned address's dirty word is somebody's live control block. Userspace
        // cannot reproduce the corruption, so it reproduces the refusal.
        assert(!test::isPinnedStaticBufferAddress(p),
               "VMSubstrate: ensureTLBEntryFresh on a PINNED static-buffer address — in "
               "the kernel this writes into another tenant's live state (R-13)");
        if (test::freshnessRecordingArmed) test::recordFreshnessCall(p);
        return false;                                      // userspace: never stale
    }

    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    // vmsmalloc DEC-050/051: the failable, runtime-callable variant. The mock
    // fails on window exhaustion and under the harness's scripted-failure hook,
    // so the RCU domain-lifecycle unwind paths are drivable here.
    void* tryReservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);
    virt_addr arenaVirtualBase(size_t index);

    void* vmsmalloc(size_t size);
    // DEC-048: same allocation path, null returns in place of the two
    // exhaustion panics. The real kernel/mm/vmsmalloc.cpp defines both and is
    // compiled into this harness, so tryMake below exercises the production
    // failable allocator rather than a userspace stand-in for it.
    void* vmsmallocTry(size_t size);
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
        void* mem = vmsmallocTry(sizeof(T));
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
        // Scripted failure for tryReservePerDomainStaticBuffer: fail every call
        // from the n'th onward (-1 disables). Lets a consumer's creation-unwind
        // path be driven without first exhausting a 64 MiB arena.
        void   setStaticReservationFailAt(long n);
        // Same idea for tryAllocPage (DEC-048): fail every call from the n'th
        // onward (-1 disables). Distinct from oracle::shouldInjectFailure —
        // this one fails the ALLOCATOR beneath tryMake rather than tryMake
        // itself, so it also drives vmsmalloc's slow-path unwind.
        void   setPageAllocFailAt(long n);

        // ─── Freshness accounting (D-049) ──────────────────────────────────
        //
        // Arm, drive the path under test, then ask whether the page a pointer
        // names was made fresh BY THIS THREAD. The usual shape is to clear
        // immediately before the access being audited, so the assertion names
        // one access rather than "something, somewhere, touched this page":
        //
        //     VS::test::armFreshnessRecording();
        //     ...reach the pointer...
        //     VS::test::clearFreshnessRecord();
        //     (void)m->baseVA;                       // the access under audit
        //     ASSERT_TRUE(VS::test::pageWasMadeFresh(m.address()));
        //
        // A `RAII` arm/disarm guard lives in the test file rather than here —
        // the mock stays a mock.
        void     armFreshnessRecording();
        void     disarmFreshnessRecording();
        // Forget every page and zero the counter; stays armed.
        void     clearFreshnessRecord();
        uint64_t freshnessCalls();
        bool     pageWasMadeFresh(const void* p);
        // Calls naming ONE page, and how many distinct pages were named. The
        // breakdown is what separates a descent's per-node cost from the single
        // call its result costs — a total alone cannot.
        uint64_t freshnessCallsForPage(const void* p);
        size_t   freshnessPagesRecorded();
        // The page record is a small fixed array — no allocation, so the
        // harness's per-test leak accounting stays honest. This says whether it
        // filled up, so a test can never pass on a TRUNCATED record, which is
        // the one way this instrumentation could lie in the safe direction.
        bool     freshnessRecordOverflowed();
    }
}

#endif // CROCOS_MOCK_RADIX_VMSUBSTRATE_H
