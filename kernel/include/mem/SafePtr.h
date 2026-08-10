//
// SafePtr — a pointer into VMSubstrate-backed memory that discharges its TLB
// freshness obligation on every access.
//
// ─── Why this is its own header ────────────────────────────────────────────
//
// Because there were THREE byte-identical copies of it: the real one, and one in
// each of the two userspace mock substrates
// (`tests/kernel/{radix,vmsmalloc}/mocks/mem/VMSubstrate.h`). Two of those are
// defensible — a kernel definition and a harness definition — but the third is a
// fork: the radix mock's own header describes itself as a "sibling" of the
// vmsmalloc one, forked in Phase 0 to add the DEC-052 poison oracle, per-type
// live accounting and fault injection. `SafePtr` was collateral, and 47 lines of
// subtle semantics were being kept in step by hand.
//
// They drifted, which is what settled this. D-072 found `SafePtr<T>` nulling the
// source on move while `SafePtr<void>` copied — one abstraction, two behaviours —
// and fixing it meant the same edit applied three times, correctly, by hand. The
// type is now defined once and the mocks include it.
//
// ─── What the mocks still own ──────────────────────────────────────────────
//
// Exactly one thing: `ensureTLBEntryFresh`. That is the whole point of a mock
// substrate and the only part that legitimately differs — the kernel invalidates
// a real TLB entry, the vmsmalloc harness is a no-op (no TLB in userspace), and
// the radix harness records the call per thread so the freshness discipline can
// be asserted (D-049). All three share the signature `bool(void*)`, which is why
// declaring it here and defining it per build works.
//
// The declaration lives here rather than in the consumer because the calls below
// are not all dependent — `SafePtr<void>::at<U>` calls it with a plain
// `unsigned char*` — so the name must be visible at DEFINITION time, not just at
// instantiation.
//

#ifndef CROCOS_MEM_SAFEPTR_H
#define CROCOS_MEM_SAFEPTR_H

#include <stddef.h>

// ─── The noexcept the contract wants, minus the one build that cannot ──────
//
// `ensureTLBEntryFresh` invalidates a TLB entry; it cannot throw, the kernel is
// built `-fno-exceptions`, and `noexcept` is the honest contract.
//
// But under `CORE_LIBRARY_TESTING` the harness's `assert` THROWS, and the radix
// mock's implementation asserts on purpose — R-13's guard, that a freshness call
// on a PINNED static-buffer address would read-modify-write another tenant's live
// data. `radix_a_safeptr_over_pinned_storage_is_refused` catches that throw via
// EXPECT_ASSERT_FAILURE, so a `noexcept` implementation turns a reported test
// failure into `std::terminate`.
//
// This is the third time this trap has been hit — see `CROCOS_RCU_NOEXCEPT` in
// core/rcu/EpochDomain.h and its reuse in rcu/RCU.h, which document the same
// reasoning. Same shape, same fix: kernel and release builds get the noexcept the
// contract specifies; the throwing-assert harness does not.
#if defined(CORE_LIBRARY_TESTING)
#define CROCOS_FRESHNESS_NOEXCEPT
#else
#define CROCOS_FRESHNESS_NOEXCEPT noexcept
#endif

namespace kernel::mm::VMSubstrate {

    // Make this CPU's mapping for the page containing `p` current.
    //
    // Returns whether it actually had to invalidate — i.e. whether this CPU's
    // mapping for that page WAS stale. Callers may ignore it; SafePtr does.
    // It exists so an instrumented consumer can distinguish "the freshness call
    // ran" from "the freshness call saved us", which is the difference between
    // exercising the DEC-047 hazard and merely walking past it (P4-ITEM-001).
    //
    // **Defined per build**, not here: kernel/mm/VMSubstrate.cpp for the real
    // one, and inline in each mock substrate for the harnesses.
    bool ensureTLBEntryFresh(void*) CROCOS_FRESHNESS_NOEXCEPT;

#if !defined(CORE_LIBRARY_TESTING)
    // The contract, asserted rather than assumed. A conditional `noexcept` is only
    // as good as the condition, and "the kernel is not a testing build" is exactly
    // the sort of thing that silently stops being true when a define leaks into a
    // target. If that happens this fires at compile time instead of quietly
    // downgrading the kernel's contract.
    static_assert(noexcept(ensureTLBEntryFresh(nullptr)),
                  "the kernel build must get the noexcept ensureTLBEntryFresh's "
                  "contract specifies — CORE_LIBRARY_TESTING has leaked into it");
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

        // ── Move is a copy, deliberately, and all five are spelled out ─────
        //
        // This is a NON-OWNING view: no destructor, unrestricted copying, and
        // `destroy()` is an explicit call rather than anything this type drives.
        // So there is nothing for a move to transfer — freshness is a property of
        // an ACCESS, not of a pointer (see the note above), so holding one confers
        // no obligation that could change hands.
        //
        // It used to null the source on move, which is unique-ownership semantics
        // on a type that has none: since copies are unrestricted it enforced no
        // uniqueness, and it made `auto b = move(a); use(a);` a null dereference
        // for `SafePtr<T>` while being perfectly fine for `SafePtr<void>` — which
        // declared a copy constructor, suppressing its implicit move, so its
        // "moves" silently bound to the copy. One abstraction, two behaviours,
        // nothing asserting either. Nothing in the tree ever moved a `SafePtr`
        // explicitly, so the nulling only ever fired on implicit moves.
        //
        // All five special members are declared here, and identically in the
        // `void` specialization, so which operations exist is readable rather than
        // inferred from what a user-declared copy constructor suppresses.
        SafePtr(const SafePtr& other) = default;
        SafePtr(SafePtr&& other) = default;
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        SafePtr<T>& operator=(SafePtr<T>&& other) = default;

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

        // All five, matching the primary template — move is a copy here too. See
        // the note there for why, and for the divergence this spelling removes.
        SafePtr(const SafePtr& other) = default;
        SafePtr(SafePtr&& other) = default;
        SafePtr<void>& operator=(const SafePtr<void>& other) = default;
        SafePtr<void>& operator=(SafePtr<void>&& other) = default;

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


}  // namespace kernel::mm::VMSubstrate

#endif  // CROCOS_MEM_SAFEPTR_H
