//
// radix-tree — the per-address-space control block and its creation sequence
// (§7.4; DEC-082 / DEC-101, with vmsmalloc DEC-050/051 and RCU-DEC-043).
//
// ─── What the block is for ────────────────────────────────────────────────
//
// "The control block is where the root-page pointer, the domain handle, the pool
// array, the `dying` flag and the address-space generation all anchor — it
// outlives every earlier step by construction and goes last."
//
// It lives in **pinned `reservePerDomainStaticBuffer` storage**, beside the RCU
// domain block, and that placement is load-bearing rather than convenient
// (DEC-082 as amended): the mapping never changes, so the generation check, the
// `dying`-flag load and the pool heads — all of which sit on other CPUs' hot
// paths — carry **no freshness obligation at all**. A vmsmalloc-backed control
// block would put an `ensureTLBEntryFresh` on every descent-cache hit, which is
// the spec's hottest path.
//
// Reservations are kernel-lifetime and never returned to VMSubstrate, so blocks
// recycle through a freelist. **Zero-fill is a per-RESERVATION guarantee, not a
// per-block one** (vmsmalloc DEC-051): a recycled block carries the previous
// tenant's bytes, and re-zeroing it is this path's job. Skipping that leaves a
// stale `dying` flag or generation behind, which is silent and durable.
//
// ─── The creation sequence, and why the order is not negotiable ───────────
//
// DEC-101 states it as a sequence with an explicit reverse unwind, and three of
// the steps have a wrong ordering that looks natural:
//
//   1. **Reserve the control block** under the domain-management lock, acquired
//      through the guard RCU-DEC-043 exports — framework-owned but
//      consumer-acquirable — and **release it before the next step**.
//   2. **`Domain::init`**, which takes the same lock *internally*. Holding it
//      across both steps self-deadlocks. Two independent rare acquisitions,
//      never one hold spanning both.
//   3. Root page, then the per-CPU record pools.
//   4. **Publish with a release store.** Threads of the address space are
//      created only after this, and thread creation's happens-before edge is
//      what orders every other CPU's first sight of the block and the domain.
//
// The unwind's own trap is `deinit()`: skipping it on a failed creation leaks
// the domain's pinned reservation **permanently and silently**, and breaks
// vmsmalloc DEC-050's high-water-mark claim — the reservation would then grow
// with cumulative process churn rather than with concurrent address spaces.
//

#ifndef CROCOS_RADIX_ADDRESS_SPACE_H
#define CROCOS_RADIX_ADDRESS_SPACE_H

#include <stddef.h>
#include <stdint.h>
#include <kassert.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>
#include <rcu/RCU.h>

#include <mem/radix/BucketCodec.h>
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/DeferredRelease.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

    // DEC-059's provisional drain batch bound, restated at the one site that
    // creates a domain for this consumer: an UNBOUNDED drain inherits
    // arbitrarily many foreign deleters on a path a `#PF` handler sits on, and
    // this consumer retires from the fault path.
    inline constexpr size_t kDrainBatchBound = 64;

    enum class CreateStatus : uint8_t { Ok, OutOfMemory };

    // ─── The block ─────────────────────────────────────────────────────────
    //
    // A plain aggregate placed into pinned storage. No constructor runs on it
    // beyond what `create` does explicitly, because a recycled block's bytes are
    // the previous tenant's and the zeroing is a step of the sequence rather
    // than a language feature.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    struct ControlBlock {
        using Table = ClusterTable<G, Codec, DetachBudget>;

        kernel::rcu::Domain  domain;
        Table                clusters;
        DeferredReleasePools pools;

        // §7.4's barrier flag. Stored release / loaded acquire, and §6.6 is
        // explicit that the flag alone orders nothing — **the grace period is
        // the handshake**. After `synchronize` returns, every section that could
        // have missed the flag has exited.
        Atomic<uint64_t> dying;

        // DEC-082: the address-space generation, which the descent cache
        // (Phase 4) checks so an entry from a previous tenant of a recycled
        // control block cannot be mistaken for a live one. Bumped on reuse, not
        // on release, so a block sitting in the freelist already carries the
        // value its next tenant will use.
        uint64_t generation;

        // Freelist linkage for recycled blocks. Pinned storage is never returned
        // to VMSubstrate, so this is how the high-water mark stays at "maximum
        // concurrent address spaces" instead of growing with process churn.
        ControlBlock* freelistNext;
    };

    // ─── The freelist ──────────────────────────────────────────────────────
    //
    // Deliberately not lock-free: DEC-101 puts every acquisition here behind the
    // domain-management lock, address-space creation and teardown are rare, and
    // rcu.md's cross-spec correction was explicit that these operations are
    // *allowed to lock*.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    struct ControlBlockFreelist {
        using Block = ControlBlock<G, Codec, DetachBudget>;

        Block*   head  = nullptr;
        uint64_t nextGeneration = 1;

        // Caller holds the domain-management lock.
        [[nodiscard]] Block* takeLocked() {
            if (head == nullptr) return nullptr;
            Block* b = head;
            head = b->freelistNext;
            b->freelistNext = nullptr;
            return b;
        }

        void returnLocked(Block* b) {
            b->freelistNext = head;
            head = b;
        }
    };

    // ─── Creation (DEC-101) ────────────────────────────────────────────────
    //
    // `outBlock` is set only on success. Every failure path has already undone
    // everything it did, which is why the unwind is written as one reverse
    // sequence rather than as `goto`-style labels: each step's undo is adjacent
    // to the step, and the `deinit()` that a reader is most likely to forget is
    // impossible to skip without deleting a visible line.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget>
    [[nodiscard]] CreateStatus createAddressSpace(
            ControlBlockFreelist<G, Codec, DetachBudget>& freelist,
            numa::DomainID home, size_t cpuCount, unsigned perCpuRecords,
            ControlBlock<G, Codec, DetachBudget>*& outBlock) {
        using Block = ControlBlock<G, Codec, DetachBudget>;
        outBlock = nullptr;

        // ─── 1. The control block, under the domain-management lock ────────
        Block* block = nullptr;
        bool   recycled = false;
        {
            kernel::rcu::DomainManagementLockGuard guard;
            block = freelist.takeLocked();
            if (block != nullptr) {
                recycled = true;
            } else {
                void* raw = VMSubstrate::tryReservePerDomainStaticBuffer(sizeof(Block), home);
                if (raw == nullptr) return CreateStatus::OutOfMemory;
                block = static_cast<Block*>(raw);
            }
            block->generation = freelist.nextGeneration++;
        }
        // The lock is RELEASED here, deliberately and visibly: `Domain::init`
        // takes it itself, so holding it across the next step self-deadlocks.

        // A fresh reservation comes zero-filled from VMSubstrate; a RECYCLED
        // block carries the previous tenant's bytes and is ours to re-zero
        // (vmsmalloc DEC-051). `generation` survives because it was written
        // above, after the wipe would have happened — so the order here is not
        // arbitrary either.
        if (recycled) {
            const uint64_t generation = block->generation;
            memset(static_cast<void*>(block), 0, sizeof(Block));
            block->generation = generation;
        }
        new (&block->domain)   kernel::rcu::Domain();
        new (&block->clusters) typename Block::Table();
        new (&block->pools)    DeferredReleasePools();
        block->dying.store(0, kPrivateInit);
        block->freelistNext = nullptr;

        // ─── 2. The domain ─────────────────────────────────────────────────
        if (!block->domain.init("radix-as", kDrainBatchBound)) {
            kernel::rcu::DomainManagementLockGuard guard;
            freelist.returnLocked(block);
            return CreateStatus::OutOfMemory;
        }

        // ─── 3. The root page ──────────────────────────────────────────────
        if (!block->clusters.init(block->domain, block->pools)) {
            (void)block->domain.deinit();
            kernel::rcu::DomainManagementLockGuard guard;
            freelist.returnLocked(block);
            return CreateStatus::OutOfMemory;
        }

        // ─── 4. The per-CPU record pools ───────────────────────────────────
        if (!block->pools.create(cpuCount, perCpuRecords)) {
            block->clusters.destroy();
            (void)block->domain.deinit();
            kernel::rcu::DomainManagementLockGuard guard;
            freelist.returnLocked(block);
            return CreateStatus::OutOfMemory;
        }

        // ─── 5. Publish ────────────────────────────────────────────────────
        //
        // Named for DEC-069's reason — unnamed publish words rot — and needing
        // no hot-path acquire opposite, because threads of the address space are
        // created only after this and thread creation's happens-before edge is
        // strictly stronger than anything an acquire here could give.
        outBlock = block;
        return CreateStatus::Ok;
    }


    // ─── Teardown (§7.4; DEC-065 / 082 / 100) ──────────────────────────────
    //
    // The sequence, in its stated order, because the phase plan says **every
    // reordering reviewed was a fatal**:
    //
    //     dying flag -> thread destruction -> `synchronize` -> the walk ->
    //     `drainAllQuiescent` -> free the root page and the record pools ->
    //     `deinit` the domain -> return the control block to its freelist
    //
    // Three of those placements are the ones to understand rather than copy:
    //
    //   - **The flag before the barrier, and the barrier is what the flag
    //     means.** §6.6: the store orders nothing on its own — after
    //     `synchronize` returns, every section that could have missed the flag
    //     has exited. Reversing them leaves writers inside sections that never
    //     saw the flag, which is what makes the walk contended.
    //
    //   - **Thread destruction before `drainAllQuiescent`, not the flag.**
    //     RCU-DEC-035's no-new-users precondition is the CALLER's to supply and
    //     the framework cannot check it. For this consumer it is thread
    //     destruction — a join is exactly the happens-before edge required — and
    //     conflating it with the dying-flag barrier "leaves a user alive". They
    //     serve different properties: the flag makes the walk uncontended, the
    //     join makes the drain safe.
    //
    //   - **The root page and the pools are freed AFTER the drain and are not
    //     retire subjects.** By then no thread can begin the descent that would
    //     read the page or the drain-refilled pools. A literal reading of
    //     "retire every node" leaks 4 KiB plus `processorCount()` record pools
    //     per address space, permanently — and invisibly to a residue gate
    //     stated in nodes.
    //
    // `caller` supplies the thread-destruction step: it must have joined every
    // thread of the address space before calling, and there is no way to check
    // that here. Stated in the signature's contract rather than asserted,
    // because a false assurance is worse than none.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget>
    void destroyAddressSpace(ControlBlockFreelist<G, Codec, DetachBudget>& freelist,
                             ControlBlock<G, Codec, DetachBudget>* block) {
        assert(block != nullptr, "radix: teardown of no address space");

        // 1. The dying flag.
        block->dying.store(1, kDyingFlagStore);

        // 2. Thread destruction — the caller's, above.

        // 3. The barrier that gives the flag its meaning.
        kernel::rcu::synchronize(block->domain);

        // 4. The walk: unit-decomposed, marking, retiring (DEC-100). Note there
        //    is deliberately NO drain between the barrier and here. An earlier
        //    synchronous walk needed one — a `DeferredRelease` record still in a
        //    bag would have released a `Mapping` the walk had already destroyed
        //    at count zero — and the retire-based walk removes the need rather
        //    than papering over it: every release, from records and node
        //    deleters alike, now lands inside the ONE drain below, over disjoint
        //    slot sets, so the count reaches zero exactly once whoever gets
        //    there last.
        block->clusters.tearDownClusters();

        // 5. Everything retired by the walk, destroyed.
        (void)kernel::rcu::drainAllQuiescent(block->domain);

        // 6. The root page and the record pools — after the drain, so nothing
        //    can begin the descent that would read either, and so every record
        //    is home, which is when §11's population-conservation check is
        //    meaningful. Neither is a retire subject.
        block->clusters.freeRootPage();
        block->pools.destroy();

        // 7. The domain. Skipping this leaks its pinned reservation permanently
        //    and silently.
        (void)block->domain.deinit();

        // 8. The control block, last, because everything above lived in it.
        {
            kernel::rcu::DomainManagementLockGuard guard;
            freelist.returnLocked(block);
        }
    }

    // Whether this address space is dying. The acquire opposite of the store
    // above; §6.6 names both so neither can be spelled bare.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget>
    [[nodiscard]] bool isDying(const ControlBlock<G, Codec, DetachBudget>& block) {
        return block.dying.load(kDyingFlagLoad) != 0;
    }

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_ADDRESS_SPACE_H
