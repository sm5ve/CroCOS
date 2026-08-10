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
#include <mem/SafePtr.h>   // the shared definition; this file supplies only ensureTLBEntryFresh
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
    inline bool ensureTLBEntryFresh(void* p) CROCOS_FRESHNESS_NOEXCEPT {
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

    // SafePtr is shared with the kernel and the other mock — <mem/SafePtr.h>,
    // included above. This file keeps only `ensureTLBEntryFresh`, which is the
    // one thing a mock substrate legitimately defines differently.

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
