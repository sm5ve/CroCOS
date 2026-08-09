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
// **What did not move with it cost a Phase 5 debugging session, and has since
// moved**: the root bucket page was an ordinary `tryMake<BucketTable>`
// allocation, so its VA recycled like any other arena page while every descent
// opened by loading a bucket word out of it — a per-touch freshness obligation
// on exactly the path this block was rearranged to keep clear, and a real stale
// read. D-051 gave it its own pinned pool at page stride (4,096 B of window per
// address space, against 8,192 B had it been folded into this block, whose
// combined 4,864 B stride would exceed a page and could not sub-page pack).
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
#include <mem/BlockPool.h>
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
    // ─── The block ─────────────────────────────────────────────────────────
    //
    // **Variable-sized: the DeferredRelease pool array lives in the tail of this
    // block's own reservation**, sized at the machine's actual CPU count rather
    // than at `arch::MAX_PROCESSOR_COUNT`. Before that, the array was a by-value
    // member and 16,448 of the block's 16,704 bytes were padding for CPUs that
    // do not exist — which on an 8-CPU machine is 98% of every process's pinned
    // reservation, and put the static-buffer window's ceiling at ~44,000
    // concurrent address spaces instead of ~500,000.
    //
    // Keeping the array *inside this reservation* rather than allocating it
    // separately is the point. The pool heads sit on every other CPU's hot path
    // (every CPU's deleters push into every CPU's pool), which is exactly why
    // DEC-082's round-4 amendment put them in pinned storage; a vmsmalloc array
    // would hand back the `ensureTLBEntryFresh` obligation that amendment
    // removed. One reservation still anchors everything.
    //
    // Cache-line aligned so the trailing array — whose entries are
    // `alignas(CACHE_LINE_SIZE)` — starts aligned at `block + 1`.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    struct alignas(arch::CACHE_LINE_SIZE) ControlBlock {
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

        // Where the pages actually are, which is not necessarily the `home` any
        // given tenant asked for: reservations are kernel-lifetime, so a block's
        // placement is fixed at its FIRST reservation and every later tenant
        // inherits it. Recorded so `returnLocked` can file the block back on the
        // list it came from instead of pooling every domain together — which is
        // what made `home` a suggestion honoured exactly once.
        numa::DomainID homeDomain;

        // The trailing pool array. Immediately after the block, cache-line
        // aligned by the `alignas` above.
        [[nodiscard]] DeferredReleasePool* poolStorage() {
            return reinterpret_cast<DeferredReleasePool*>(this + 1);
        }

        [[nodiscard]] static size_t reservationBytes(size_t cpus) {
            return sizeof(ControlBlock) + deferredReleasePoolBytes(cpus);
        }
    };

    // ─── The block store ───────────────────────────────────────────────────
    //
    // Was a hand-rolled freelist over whole `reservePerDomainStaticBuffer`
    // reservations. It is now a `PinnedBlockPool` plus a generation counter, and
    // the three things that disappeared with it are the point:
    //
    //   - **the per-domain partitioning**, which this had to grow after a block
    //     recycled onto the wrong NUMA node (D-045);
    //   - **the size check**, which existed because variable-sized blocks made
    //     an undersized reuse an out-of-bounds write — the pool is fixed-stride,
    //     so the whole class of bug is gone rather than guarded;
    //   - **the page minimum**. Every block used to cost a 4 KiB page; at
    //     `sizeof(ControlBlock)` + 8 CPUs of pools = 768 B, five now share one.
    //
    // What stays here is the part that is genuinely radix's: the **generation**,
    // which is an address-space identity concept (DEC-082) that the descent
    // cache compares against, not a property of the storage.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>
    struct ControlBlockStore {
        using Block = ControlBlock<G, Codec, DetachBudget>;

        PinnedBlockPool pool;
        // ─── The root bucket page's pool (D-051) ───────────────────────────
        //
        // A SECOND instance rather than a bigger stride on the first, and the
        // difference is 2x of the window: folding the page into the control
        // block makes the stride 4,864 B — larger than a page, so it cannot
        // sub-page pack AND loses 41% to page rounding, 8,192 B per address
        // space. At its own stride the page packs exactly: 4,096 B.
        //
        // What the move buys is not packing but the **mapping**. On vmsmalloc
        // the root page's VA recycles with every address-space lifecycle, so
        // every descent's opening bucket read owed a freshness call and a real
        // stale-read bug came out of it (D-039). Pinned, that obligation is gone
        // at the source — which is DEC-082's own argument for the control block,
        // applied to the one per-address-space allocation that did not move with
        // it and is read more often than anything that did.
        PinnedBlockPool rootPages;
        uint64_t        nextGeneration = 1;

        // `cpus` fixes the control-block stride for this store's lifetime. In
        // the kernel that is `arch::processorCount()`, constant after boot; the
        // harness varies it per fixture and gets one store per fixture. The root
        // page's stride is a constant — it is exactly one page by construction
        // (DEC-102 derives the bucket count from the page size).
        void init(size_t cpus) {
            pool.init(Block::reservationBytes(cpus));
            rootPages.init(sizeof(BucketTable));
        }

        // Caller holds the domain-management lock — the pool's contract, which
        // is in turn `reserveStaticBufferImpl`'s.
        [[nodiscard]] PoolBlock takeLocked(numa::DomainID home) {
            return pool.allocateLocked(home);
        }

        void returnLocked(Block* b) {
            pool.freeLocked(PoolBlock{static_cast<void*>(b), b->homeDomain});
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
            ControlBlockStore<G, Codec, DetachBudget>& store,
            numa::DomainID home, size_t cpuCount, unsigned perCpuRecords,
            ControlBlock<G, Codec, DetachBudget>*& outBlock) {
        using Block = ControlBlock<G, Codec, DetachBudget>;
        outBlock = nullptr;

        // ─── 1. The control block, under the domain-management lock ────────
        Block*         block = nullptr;
        numa::DomainID placedOn = home;
        uint64_t       generation = 0;
        {
            kernel::rcu::DomainManagementLockGuard guard;
            // The store's stride is fixed by its first caller and covers the
            // block AND its trailing pool array — see ControlBlock's note on why
            // the array is in the same storage rather than allocated separately.
            //
            // This assert replaces the size-checking scan the hand-rolled
            // freelist needed. A fixed-stride pool cannot hand back an
            // undersized block, so the only remaining way to get one is to ask a
            // store for more CPUs than it was built for — which is a caller
            // error and fails loudly here rather than becoming an out-of-bounds
            // write into the next block. Unreachable in the kernel, where
            // `processorCount()` is constant after boot.
            if (!store.pool.ready()) store.init(cpuCount);
            assert(Block::reservationBytes(cpuCount) <= store.pool.blockStride(),
                   "radix: this ControlBlockStore was built for fewer CPUs than this "
                   "address space needs — its blocks' trailing pool arrays are too short");
            const PoolBlock got = store.takeLocked(home);
            if (!got) return CreateStatus::OutOfMemory;
            block      = static_cast<Block*>(got.ptr);
            placedOn   = got.domain;
            generation = store.nextGeneration++;
        }
        // The lock is RELEASED here, deliberately and visibly: `Domain::init`
        // takes it itself, so holding it across the next step self-deadlocks.

        // A fresh reservation comes zero-filled from VMSubstrate; a RECYCLED
        // block carries the previous tenant's bytes and is ours to re-zero
        // (vmsmalloc DEC-051). `generation` survives because it was written
        // above, after the wipe would have happened — so the order here is not
        // arbitrary either.
        {
            // Unconditionally, whatever the block's provenance.
            //
            // **Defence in depth, and honestly not load-bearing today**:
            // `pools.destroy()` already drains every record and resets each
            // pool's head, population, depth and counters before the block is
            // returned, so the trailing array is clean on arrival. Shrinking
            // this to `sizeof(Block)` was mutation-tested and no test noticed.
            //
            // Kept anyway, and the reason is vmsmalloc DEC-051's: zero-fill is a
            // per-RESERVATION guarantee, not a per-block one, so a recycled
            // block's contents are the caller's problem. Making cleanliness a
            // property of CREATION rather than of `destroy()` happening to stay
            // thorough is what stops a field added to the pool later — and not
            // reset there — from becoming a live record list handed to a process
            // that does not own it.
            //
            // Unconditional now that the pool owns reuse: the caller cannot tell
            // a recycled block from a fresh one and should not have to. That is
            // RCU-DEC-043(i)'s formulation — zeroing at the consumer "makes every
            // init valid regardless of block provenance, which is the only form
            // of the rule that does not depend on how the caller obtained its
            // storage" — and it is also what a fresh page's zero-fill makes free.
            memset(static_cast<void*>(block), 0, store.pool.blockStride());
        }
        // Written after the wipe. `homeDomain` is where the pages actually are,
        // which the pool reports because a preferred-domain request can be
        // satisfied from another domain's free list.
        block->generation = generation;
        block->homeDomain = placedOn;
        new (&block->domain)   kernel::rcu::Domain();
        new (&block->clusters) typename Block::Table();
        new (&block->pools)    DeferredReleasePools();
        block->dying.store(0, kPrivateInit);

        // ─── 2. The domain ─────────────────────────────────────────────────
        if (!block->domain.init("radix-as", kDrainBatchBound)) {
            kernel::rcu::DomainManagementLockGuard guard;
            store.returnLocked(block);
            return CreateStatus::OutOfMemory;
        }

        // ─── 3. The root page ──────────────────────────────────────────────
        //
        // Its own lock scope, and that is forced rather than tidy: the page now
        // comes from a pinned pool, whose contract is that the caller holds the
        // domain-management lock — but step 2's `Domain::init` takes that lock
        // ITSELF, so a hold spanning both self-deadlocks (RCU-DEC-043). Placed on
        // the domain the control block actually landed on, so an address space's
        // pinned storage stays together.
        bool rootPageOk = false;
        {
            kernel::rcu::DomainManagementLockGuard guard;
            rootPageOk = block->clusters.initLocked(block->domain, block->pools,
                                                    store.rootPages, block->homeDomain);
        }
        if (!rootPageOk) {
            (void)block->domain.deinit();
            kernel::rcu::DomainManagementLockGuard guard;
            store.returnLocked(block);
            return CreateStatus::OutOfMemory;
        }

        // ─── 4. The per-CPU record pools ───────────────────────────────────
        // The reserve is sized at the per-operation ceiling and the per-CPU
        // pools are not: that split is ITEM-084's answer, and which of the two
        // carries §7.1's termination argument is stated on `refillFromReserve`.
        if (!block->pools.create(block->poolStorage(), cpuCount, perCpuRecords,
                                 deferredReleaseBound(G))) {
            block->clusters.destroy();
            (void)block->domain.deinit();
            kernel::rcu::DomainManagementLockGuard guard;
            store.returnLocked(block);
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
    void destroyAddressSpace(ControlBlockStore<G, Codec, DetachBudget>& store,
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
        //    The root page goes back to a pinned pool, so it takes the
        //    domain-management lock — and NOT the same hold as step 8's control
        //    block return, because `Domain::deinit` sits between them and takes
        //    that lock itself.
        {
            kernel::rcu::DomainManagementLockGuard guard;
            block->clusters.freeRootPageLocked();
        }
        block->pools.destroy();

        // 7. The domain. Skipping this leaks its pinned reservation permanently
        //    and silently.
        (void)block->domain.deinit();

        // 8. The control block, last, because everything above lived in it.
        {
            kernel::rcu::DomainManagementLockGuard guard;
            store.returnLocked(block);
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
