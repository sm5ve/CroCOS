//
// Created by Spencer Martin on 5/28/26.
//
// TreiberStack / ChainedTreiberStack: lock-free intrusive concurrent stacks
// with ABA-safe tagged-head encoding. Phase 4 of the vmsmalloc design;
// supplies the magazine-flush primitive for vmsmalloc's per-domain Treiber
// stacks (parent-spec DEC-002, DEC-034, DEC-041).
//
// Two variants:
//   TreiberStack<T, LinkageExtractor, HeadEncoding>
//     A classic intrusive Treiber stack. push(node) is a single-element CAS;
//     pop() returns the top node or nullptr.
//
//   ChainedTreiberStack<T, HeadLinkage, ChainLinkage, HeadEncoding, Hooks>
//     Every stack entry is the head of a chain (linked via ChainLinkage's
//     getChainNext / chainNext). push(element) extends the current top chain
//     up to a runtime-tunable maxChainLength or starts a new singleton chain
//     (used by the cross-domain singleton-free path). pushChain(head, depth)
//     publishes a pre-built chain as a new top in one CAS (matches parent-spec
//     DEC-034 magazine-flush semantics; ignores maxChainLength). pop() detaches
//     the entire top chain in one CAS and returns {head, depth}.
//
// Memory ordering (parent-spec DEC-042):
//   - Push CAS: RELEASE on success, RELAXED on failure.
//   - Pop initial / retry load: ACQUIRE. (Load-bearing on ARMv8/RISC-V.)
//   - Pop CAS: ACQUIRE on success, RELAXED on failure.
//   - chainNext / chainDepth fields on chain heads: RELAXED. The cross-CPU
//     happens-before edge comes from the popper's ACQUIRE-load of the head
//     synchronizing-with the pusher's RELEASE-CAS.
// Every CAS site references named MemoryOrder constants defined below;
// downgrading any of them is a one-point edit and an audit trigger.
//
// Hooks (template-parameter extension points):
//   - onPreTouch(topPtr): fires AFTER the head acquire-load but BEFORE any
//     read of *topPtr. vmsmalloc uses this to call
//     VMSubstrate::ensureTLBEntryFresh(topPtr) (parent-spec DEC-040 amended);
//     without it, a stale TLB mapping on this CPU could make the read of
//     topPtr->next / topPtr->chainDepth see content from a previous physical
//     backing of the page, which would then be published as the new shared-
//     stack head via the CAS.
//   - onCasFailure(): fires per CAS-loop retry. Tuning policies use this as
//     a contention signal.
//   - onEmptyStack(): fires inside pop() when the head pointer is null.
//     Tuning policies use this as a starvation signal.
// Default Hooks = NoopHooks (empty struct, zero-overhead via [[no_unique_address]]).
//
// Head encoding (template-parameter policy):
//   Core ships Uint128HeadEncoding<T> (16-byte storage: 8-byte ptr + 8-byte tag)
//   as a default for consumers without a fixed-VA-window constraint. vmsmalloc
//   supplies its own DEC-015 64-bit packed encoding (27-bit page-offset + 37-bit
//   counter) in Phase 5.
//
// Reclamation:
//   Out of scope. ABA safety is provided by the tag; node lifetime is the
//   consumer's responsibility. For vmsmalloc, slabs live forever per DEC-002.
//

#ifndef CROCOS_TREIBERSTACK_H
#define CROCOS_TREIBERSTACK_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <core/atomic.h>
#include <core/utility.h>
#include <core/TypeTraits.h>

namespace Core {

    // ─── Linkage extractor concept (basic Treiber) ───────────────────────────
    //
    // The Treiber linkage is the "next chain head on the shared stack" pointer.
    // For TreiberStack<T>, every node uses this linkage; for ChainedTreiberStack
    // it is defined only on chain heads (the chainNext linkage is the separate
    // ChainedTreiberChainLinkage concept).
    template <typename T, typename Extractor>
    concept TreiberLinkageExtractor = requires(T& node, T* p) {
        { Extractor::getNext(node) } -> convertible_to<T*>;
        { Extractor::setNext(node, p) };
    };

    // ─── Head encoding policy concept ────────────────────────────────────────
    //
    // The HeadEncoding policy defines the on-the-wire head layout. Stateless,
    // static-method policy struct (extractor pattern; cf. P4-DEC-001). Supplies
    // an opaque Storage type — typically uint64_t (for fixed-VA-window encodings
    // like DEC-015) or __uint128_t (the default for generic consumers).
    //
    // Zero-init invariant: unpack of a zero-initialized Storage value must
    // yield a nullptr pointer, so default-constructed stacks are empty. Not
    // checkable via concept; the Treiber stack body relies on the contract.
    template <typename T, typename Policy>
    concept TreiberHeadEncoding = requires(typename Policy::Storage s,
                                           typename Policy::Tag t,
                                           T* p) {
        typename Policy::Storage;
        typename Policy::Tag;
        { Policy::pack(p, t) } -> convertible_to<typename Policy::Storage>;
        { Policy::unpackPointer(s) } -> convertible_to<T*>;
        { Policy::unpackTag(s) } -> convertible_to<typename Policy::Tag>;
        { Policy::advanceTag(t) } -> convertible_to<typename Policy::Tag>;
        requires is_trivially_copyable_v<typename Policy::Storage>;
    };

    // ─── Hooks policy concept (extension points; default = no-ops) ───────────
    //
    // Hooks let consumers inject behavior at specific lifecycle points without
    // exposing internal state. Three hook points: onPreTouch, onCasFailure,
    // onEmptyStack — see file-level documentation block for semantics.
    template <typename T, typename H>
    concept TreiberHooks = requires(H& h, T* p) {
        { h.onPreTouch(p) };
        { h.onCasFailure() };
        { h.onEmptyStack() };
    };

    struct NoopHooks {
        template <typename T> void onPreTouch(T*) const noexcept {}
        void onCasFailure() const noexcept {}
        void onEmptyStack() const noexcept {}
    };

    // ─── Default head encoding for generic consumers ─────────────────────────
    //
    // 128-bit head: low 64 bits = pointer; high 64 bits = monotonically
    // advancing tag. Push-only counter advance (parent-spec DEC-015 rule —
    // a pop that bumped the tag would invalidate the wraparound analysis).
    //
    // The 64-bit tag wraps in ~584 years at 10⁹ pushes/sec. Practical infinity.
    template <typename T>
    struct Uint128HeadEncoding {
        struct alignas(16) Storage {
            uintptr_t ptr;
            uint64_t  tag;
            // Equality is used by Atomic<Storage>::compare_exchange under the
            // hood (via __atomic_compare_exchange_n); the struct is trivially
            // copyable, so the intrinsic bit-compares fine.
        };
        using Tag = uint64_t;

        static constexpr Storage pack(T* p, Tag t) {
            return { reinterpret_cast<uintptr_t>(p), t };
        }
        static constexpr T* unpackPointer(Storage s) {
            return reinterpret_cast<T*>(s.ptr);
        }
        static constexpr Tag unpackTag(Storage s) { return s.tag; }
        static constexpr Tag advanceTag(Tag t)   { return t + 1; }

        static_assert(sizeof(Storage) == 16);
        // The `|| sizeof(T) == 0` term makes the condition value-dependent on
        // T, deferring the check to instantiation. Without it, GCC's
        // -Wtemplate-body evaluates the non-dependent __atomic_is_lock_free at
        // parse time, which breaks any freestanding TU that merely *includes*
        // this header on a target without 16-byte lock-free CAS (e.g. the
        // CroCOS kernel, which deliberately uses a 64-bit HeadEncoding per
        // DEC-015 and never instantiates Uint128HeadEncoding). The assertion
        // still fires if a consumer actually instantiates this encoding on
        // such a target.
        static_assert(__atomic_is_lock_free(16, nullptr) || sizeof(T) == 0,
                      "Uint128HeadEncoding requires native 16-byte lock-free CAS "
                      "(cmpxchg16b on AMD64; LDXP/STXP or CASP on ARMv8). "
                      "Supply a custom HeadEncoding policy if your target lacks it.");
    };

    // ─── Memory-ordering constants (DEC-042 pinned) ──────────────────────────
    //
    // Every CAS site in this file references these by name. Any downgrade
    // (e.g. push CAS to RELAXED) is a one-point edit and an audit trigger.
    // Do NOT collapse kTreiberPopLoadOrder to RELAXED — x86 TSO would tolerate
    // it but ARMv8/RISC-V would silently break (the exact failure mode DEC-042
    // is portably correct against).
    inline constexpr MemoryOrder kTreiberPushOrder       = RELEASE;   // DEC-042 #1
    inline constexpr MemoryOrder kTreiberPushLoadOrder   = RELAXED;   // DEC-042 #1
    inline constexpr MemoryOrder kTreiberPopLoadOrder    = ACQUIRE;   // DEC-042 #2
    inline constexpr MemoryOrder kTreiberPopCASOrder     = ACQUIRE;   // DEC-042 #2
    inline constexpr MemoryOrder kTreiberCASFailureOrder = RELAXED;   // DEC-042 #1,#2
    inline constexpr MemoryOrder kChainLinkOrder         = RELAXED;   // DEC-042 #3

    // ─── ChainLinkage concept ────────────────────────────────────────────────
    //
    // ChainedTreiberStack's ChainLinkage parameter must supply both the
    // intra-chain pointer (getChainNext/setChainNext) and the chainDepth
    // field accessors. The shared-stack linkage uses a separate HeadLinkage
    // parameter that satisfies TreiberLinkageExtractor.
    template <typename T, typename Extractor>
    concept ChainedTreiberChainLinkage = requires(T& node, T* p, uint32_t d) {
        { Extractor::getChainNext(node) } -> convertible_to<T*>;
        { Extractor::setChainNext(node, p) };
        { Extractor::getChainDepth(node) } -> convertible_to<uint32_t>;
        { Extractor::setChainDepth(node, d) };
    };

    // ─── Basic Treiber stack ─────────────────────────────────────────────────
    //
    // Concurrent intrusive stack. push(node) pushes a single node; pop()
    // returns the top node (or nullptr if empty).
    //
    // Memory model: see DEC-042. Push CAS is RELEASE; pop initial load is
    // ACQUIRE (load-bearing on weak architectures).
    template <typename T, typename LinkageExtractor, typename HeadEncoding>
    requires TreiberLinkageExtractor<T, LinkageExtractor>
          && TreiberHeadEncoding<T, HeadEncoding>
    class TreiberStack {
        using Storage = typename HeadEncoding::Storage;
        // Cache-line isolation: keep the head off the line shared with the
        // surrounding container. (Hardcoded 64 per CLAUDE.md — there is no
        // codebase-wide arch::cacheLineSize constant in CroCOS today.)
        alignas(64) Atomic<Storage> head{};   // zero-init == empty per policy contract

    public:
        TreiberStack() = default;
        TreiberStack(const TreiberStack&) = delete;
        TreiberStack& operator=(const TreiberStack&) = delete;

        // push(node): single-element push. Performs one CAS loop on the head.
        // node->next is written inside the loop (consumer's prior store, if
        // any, is overwritten). On success, every prior write to *node is
        // published to any future pop().
        void push(T& node) {
            Storage old = head.load(kTreiberPushLoadOrder);
            Storage neu;
            while (true) {
                T* oldPtr = HeadEncoding::unpackPointer(old);
                LinkageExtractor::setNext(node, oldPtr);
                auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
                neu = HeadEncoding::pack(&node, newTag);
                if (head.compare_exchange(old, neu,
                                          kTreiberPushOrder,           // DEC-042 #1: RELEASE
                                          kTreiberCASFailureOrder)) {  // DEC-042 #1: RELAXED
                    return;
                }
                // `old` already updated by compare_exchange on failure.
            }
        }

        // pop(): single-element pop. Returns the popped node, or nullptr if empty.
        // Reads node->next inside the loop after an ACQUIRE-load of head.
        T* pop() {
            Storage old = head.load(kTreiberPopLoadOrder);            // DEC-042 #2: ACQUIRE
            while (true) {
                T* oldPtr = HeadEncoding::unpackPointer(old);
                if (oldPtr == nullptr) return nullptr;
                // Speculative read of next; CAS will reject if head has changed.
                T* nextPtr = LinkageExtractor::getNext(*oldPtr);
                auto oldTag = HeadEncoding::unpackTag(old);           // pop carries tag (DEC-015)
                Storage neu = HeadEncoding::pack(nextPtr, oldTag);
                if (head.compare_exchange(old, neu,
                                          kTreiberPopCASOrder,        // DEC-042 #2: ACQUIRE
                                          kTreiberCASFailureOrder)) { // DEC-042 #2: RELAXED
                    return oldPtr;
                }
                // On failure: re-load with ACQUIRE for the next attempt.
                old = head.load(kTreiberPopLoadOrder);
            }
        }

        // Snapshot accessor: lockless peek. Returns nullptr if empty; the
        // returned pointer may be stale by the time the caller dereferences it.
        // Safe only for diagnostic use (e.g. asserting empty at teardown).
        T* peek() const {
            Storage cur = head.load(kTreiberPopLoadOrder);
            return HeadEncoding::unpackPointer(cur);
        }

        [[nodiscard]] bool empty() const { return peek() == nullptr; }
    };

    // ─── Chained Treiber stack ───────────────────────────────────────────────
    //
    // Like TreiberStack, but every shared-stack head bears an intra-chain linkage
    // (ChainLinkage::getChainNext / setChainNext) and an explicit chainDepth
    // (consumer field on T, read via ChainLinkage::getChainDepth /
    // setChainDepth). HeadLinkage parameterizes the shared-stack next pointer
    // (separate from chainNext).
    //
    // push(element) semantics:
    //   - Stack empty: publish element as a new singleton chain (chainNext=null,
    //     chainDepth=1, next=null).
    //   - Top chain at maxChainLength: publish element as a new singleton chain
    //     above the existing top (chainNext=null, chainDepth=1, next=oldTop).
    //   - Otherwise: extend the top chain (chainNext=oldTop, chainDepth=
    //     oldTop.chainDepth+1, next=oldTop.next).
    //   Exactly one CAS publishes the new head.
    //
    // pushChain(chainHead, depth) semantics:
    //   - Caller passes a pre-built chain (chainNext linkage and chainDepth
    //     already set; depth >= 1; bottom chainNext == nullptr).
    //   - One CAS publishes the chain as a new top, ALWAYS as a new chain —
    //     never extends the existing top. maxChainLength is ignored.
    //   - This is the magazine-flush path per parent-spec DEC-034.
    //
    // pop() semantics:
    //   - One CAS atomically detaches the entire top chain. Returns
    //     {head, depth}, or {nullptr, 0} if empty.
    //   - Caller is sole owner of the popped chain head and all chainNext-
    //     reachable nodes; the stack will not touch them again.
    //
    // maxChainLength is mutable at runtime (RELAXED-atomic). Reducing it does
    // not affect existing in-stack chains; only future pushes are bounded.
    template <typename T,
              typename HeadLinkage,
              typename ChainLinkage,
              typename HeadEncoding,
              typename Hooks = NoopHooks>
    requires TreiberLinkageExtractor<T, HeadLinkage>
          && ChainedTreiberChainLinkage<T, ChainLinkage>
          && TreiberHeadEncoding<T, HeadEncoding>
          && TreiberHooks<T, Hooks>
    class ChainedTreiberStack {
        using Storage = typename HeadEncoding::Storage;
        alignas(64) Atomic<Storage>  head{};
        alignas(64) Atomic<uint32_t> maxChainLength;
        [[no_unique_address]] Hooks  hooks{};   // zero-size for NoopHooks

    public:
        struct PoppedChain { T* head; uint32_t depth; };

        explicit ChainedTreiberStack(uint32_t initialMaxChainLength)
            requires __is_constructible(Hooks)
            : maxChainLength(initialMaxChainLength) {}

        ChainedTreiberStack(uint32_t initialMaxChainLength, Hooks h)
            : maxChainLength(initialMaxChainLength), hooks(move(h)) {}

        ChainedTreiberStack(const ChainedTreiberStack&) = delete;
        ChainedTreiberStack& operator=(const ChainedTreiberStack&) = delete;

        // Push a single element. May extend the existing top chain or push a
        // new singleton; either way, exactly one CAS publishes the new head.
        void push(T& element) {
            Storage old = head.load(kTreiberPushLoadOrder);
            uint32_t kmax = maxChainLength.load(RELAXED);
            Storage neu;
            while (true) {
                T* topPtr = HeadEncoding::unpackPointer(old);
                if (topPtr == nullptr) {
                    // New singleton chain on empty stack.
                    ChainLinkage::setChainNext(element, nullptr);
                    ChainLinkage::setChainDepth(element, 1);
                    HeadLinkage::setNext(element, nullptr);
                } else {
                    // Hook fires before any read of *topPtr — vmsmalloc's hook
                    // calls ensureTLBEntryFresh(topPtr) here (parent-spec
                    // DEC-040, amended). The read of getChainDepth(*topPtr) /
                    // getNext(*topPtr) below would otherwise go through this
                    // CPU's potentially-stale TLB.
                    hooks.onPreTouch(topPtr);
                    uint32_t topDepth = ChainLinkage::getChainDepth(*topPtr);
                    if (topDepth >= kmax) {
                        // New singleton chain (top at max length).
                        ChainLinkage::setChainNext(element, nullptr);
                        ChainLinkage::setChainDepth(element, 1);
                        HeadLinkage::setNext(element, topPtr);
                    } else {
                        // Extend the existing top chain. DEC-042 #3: RELAXED
                        // reads of topPtr's fields are sufficient because the
                        // ACQUIRE-load of head that returned topPtr already
                        // synchronizes-with the pusher's RELEASE-CAS.
                        ChainLinkage::setChainNext(element, topPtr);
                        ChainLinkage::setChainDepth(element, topDepth + 1);
                        HeadLinkage::setNext(element, HeadLinkage::getNext(*topPtr));
                    }
                }
                auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
                neu = HeadEncoding::pack(&element, newTag);
                if (head.compare_exchange(old, neu,
                                          kTreiberPushOrder,           // DEC-042 #1: RELEASE
                                          kTreiberCASFailureOrder)) {  // DEC-042 #1: RELAXED
                    return;
                }
                hooks.onCasFailure();
                // `old` already updated by compare_exchange on failure.
            }
        }

        // Push a pre-built chain as a new top chain (single CAS, no extend).
        // The caller has already set chainNext linkage through the chain bottom
        // and chainDepth = depth on chainHead. maxChainLength is not consulted.
        void pushChain(T& chainHead, uint32_t depth) {
            // Debug-asserted caller invariants:
            //   - depth >= 1
            //   - ChainLinkage::getChainDepth(chainHead) == depth
            // Chain-walk validation (chainNext terminates at exactly depth-1
            // steps) is intentionally not asserted here — O(depth) per push is
            // too costly for debug; the Failure Modes section pins this as a
            // caller bug undetected by the primitive.
            assert(depth >= 1, "ChainedTreiberStack::pushChain: depth must be >= 1");
            assert(ChainLinkage::getChainDepth(chainHead) == depth,
                   "ChainedTreiberStack::pushChain: chainHead.chainDepth mismatch");
            (void)depth;  // depth is only used in the asserts above; in release
                          // builds the field is already set on chainHead.

            // No onPreTouch hook fires here — pushChain doesn't read *oldPtr's
            // fields, it only writes chainHead->next = oldPtr.
            Storage old = head.load(kTreiberPushLoadOrder);
            Storage neu;
            while (true) {
                T* oldPtr = HeadEncoding::unpackPointer(old);
                HeadLinkage::setNext(chainHead, oldPtr);
                auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
                neu = HeadEncoding::pack(&chainHead, newTag);
                if (head.compare_exchange(old, neu,
                                          kTreiberPushOrder,           // DEC-042 #1: RELEASE
                                          kTreiberCASFailureOrder)) {  // DEC-042 #1: RELAXED
                    return;
                }
                hooks.onCasFailure();
            }
        }

        // Atomically detach the entire top chain. Returns {nullptr, 0} on empty.
        PoppedChain pop() {
            Storage old = head.load(kTreiberPopLoadOrder);          // DEC-042 #2: ACQUIRE
            while (true) {
                T* topPtr = HeadEncoding::unpackPointer(old);
                if (topPtr == nullptr) {
                    hooks.onEmptyStack();
                    return { nullptr, 0 };
                }
                // Hook fires before any read of *topPtr — vmsmalloc's hook
                // calls ensureTLBEntryFresh(topPtr) here. The subsequent read
                // of getChainDepth(*topPtr) / getNext(*topPtr) is not just
                // consumed locally — getNext is published as the new shared-
                // stack head via the CAS, so a TLB-stale read would corrupt
                // the structure visibly to all CPUs. The acquire-load on head
                // synchronizes value visibility through the current PTE but
                // does NOT refresh this CPU's TLB mapping.
                hooks.onPreTouch(topPtr);
                uint32_t depth = ChainLinkage::getChainDepth(*topPtr);
                T* nextStackHead = HeadLinkage::getNext(*topPtr);
                auto oldTag = HeadEncoding::unpackTag(old);          // pop carries tag (DEC-015)
                Storage neu = HeadEncoding::pack(nextStackHead, oldTag);
                if (head.compare_exchange(old, neu,
                                          kTreiberPopCASOrder,        // DEC-042 #2: ACQUIRE
                                          kTreiberCASFailureOrder)) { // DEC-042 #2: RELAXED
                    return { topPtr, depth };
                }
                hooks.onCasFailure();
                old = head.load(kTreiberPopLoadOrder);
            }
        }

        // Runtime knob. Cheap RELAXED read on every push; cheap RELAXED store
        // by a tuning thread. New value visible on next push.
        void     setMaxChainLength(uint32_t k) {
            maxChainLength.store(k, RELAXED);
        }
        uint32_t getMaxChainLength() const {
            return maxChainLength.load(RELAXED);
        }

        // Diagnostic peek; returns {nullptr, 0} on empty. Depth is a snapshot
        // subject to concurrent invalidation.
        PoppedChain peek() const {
            Storage cur = head.load(kTreiberPopLoadOrder);
            T* topPtr = HeadEncoding::unpackPointer(cur);
            if (topPtr == nullptr) return { nullptr, 0 };
            uint32_t depth = ChainLinkage::getChainDepth(*topPtr);
            return { topPtr, depth };
        }

        [[nodiscard]] bool empty() const {
            Storage cur = head.load(kTreiberPopLoadOrder);
            return HeadEncoding::unpackPointer(cur) == nullptr;
        }
    };

}  // namespace Core

#endif //CROCOS_TREIBERSTACK_H
