//
// A suballocator over VMSubstrate's pinned static-buffer region.
//
// ─── Why this exists ──────────────────────────────────────────────────────
//
// `reservePerDomainStaticBuffer` is a bump pointer with **no free path and page
// granularity**, and both halves cost real memory. Measured on an 8-CPU machine:
// the radix control block wants 768 B and takes a 4 KiB page (81% wasted); RCU's
// reader slots want 1,024 B and take a page (75% wasted). Sizing either request
// smaller saves nothing, because the bump pointer advances by whole pages
// regardless — the waste is in the granularity, not in the callers.
//
// And with no free path, every consumer that needs reuse hand-rolls a freelist
// over the blocks it was handed. Two have: `kernel::rcu`'s `gFreeSlotBlocks` and
// the radix tree's `ControlBlockFreelist`. They are the same structure written
// twice, and the radix one had to grow NUMA partitioning and a size check that
// the RCU one did not need — so the second author got a different subset right,
// which is exactly the argument for writing it once here.
//
// ─── Why it needs no shootdown, and never will ────────────────────────────
//
// The one thing this must not do is **unmap a page**, and it never needs to.
//
// `reserveStaticBufferImpl`'s safety argument (DEC-051b) is that every page-table
// entry it writes "transitions not-present -> present EXACTLY ONCE and never
// changes". That immutability is precisely why pinned storage carries no
// `ensureTLBEntryFresh` obligation while vmsmalloc memory does — and it is
// exactly what a free path would break, since unmapping is a present ->
// not-present transition that x86 *does* cache.
//
// Handing a block to a new tenant does not touch a PTE at all. The VA -> physical
// mapping is unchanged and only the bytes differ, so every CPU's cached
// translation stays *correct*. **Reuse is free; only returning pages to the
// PageAllocator would break the invariant**, and no consumer has asked for that.
// So this pool grows and never shrinks, which is the deliberate trade.
//
// ─── Deliberately not fast ────────────────────────────────────────────────
//
// A per-domain intrusive freelist under the caller's lock. Address-space
// creation and teardown are rare — the operations this serves are `fork`,
// `exec` and process exit, not anything on a fault path — and rcu.md's
// cross-spec correction was explicit that these operations are allowed to lock.
// A block is one pop and one push.
//
// ─── What it does not do ──────────────────────────────────────────────────
//
// **It does not zero.** Zero-fill is a per-RESERVATION guarantee (vmsmalloc
// DEC-051), so a fresh page arrives zeroed and a recycled block carries the
// previous tenant's bytes. Both existing consumers already re-zero on their own
// init path, and RCU-DEC-043(i) states the reason better than this comment
// could: zeroing at the consumer "makes every init valid regardless of block
// provenance, which is the only form of the rule that does not depend on how the
// caller obtained its storage." Zeroing here would make consumers' correctness
// depend on which allocator they happened to use.
//
// **It does not serve multi-page blocks with per-page NUMA placement.** RCU's
// slot block places page `p` on the domain of the CPUs whose slots it holds, and
// additionally depends on consecutive reservations returning consecutive VAs —
// a property of the bump allocator that a shared pool would break by
// interleaving carves. A consumer with that shape keeps reserving directly.
//
// Note what that does NOT exclude, because RCU is the example (radix D-048):
// both obstacles are statements about pages, so both go vacuous at ONE page.
// RCU therefore draws single-page slot arrays — every machine up to 32 CPUs —
// from a pool, and keeps its own path only for the multi-page case. A consumer
// whose block spans pages only SOMETIMES is not disqualified; it is two cases.
//

#ifndef CROCOS_PINNED_BLOCK_POOL_H
#define CROCOS_PINNED_BLOCK_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <kassert.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>

namespace kernel::mm {

    // A block, together with the domain it actually landed on — which is not
    // necessarily the one that was asked for, because the pool prefers reusing a
    // remote block over burning window on a fresh page (see `allocateLocked`).
    //
    // Returning the pair rather than a bare pointer is what stops every consumer
    // reinventing radix's `homeDomain` field: the allocator knows where the
    // memory is, so it is the allocator's job to say.
    struct PinnedBlock {
        void*          ptr    = nullptr;
        numa::DomainID domain = numa::DomainID{0};

        explicit operator bool() const { return ptr != nullptr; }
    };

    class PinnedBlockPool {
    public:
        // Inert: no global constructor may touch VMSubstrate, which does not
        // exist during `cpp_init` (see [[project_crocos_static_init_rule]] —
        // PITEventSource panicked exactly this way).
        constexpr PinnedBlockPool() = default;

        PinnedBlockPool(const PinnedBlockPool&)            = delete;
        PinnedBlockPool& operator=(const PinnedBlockPool&) = delete;

        // `blockBytes` is rounded up to a cache line, and that rounding is not
        // tidiness: adjacent blocks belong to DIFFERENT address spaces, and the
        // fields these hold — a generation, a dying flag, per-CPU pool heads —
        // are read by every CPU. Two processes sharing a cache line would false-
        // share on the hottest per-address-space reads in the system.
        void init(size_t blockBytes) {
            assert(blockBytes > 0, "PinnedBlockPool: zero-sized block");
            assert(!initialized, "PinnedBlockPool: init called twice");
            stride = roundUpTo(blockBytes, arch::CACHE_LINE_SIZE);
            assert(stride >= sizeof(FreeNode),
                   "PinnedBlockPool: a block must be large enough to hold the intrusive "
                   "freelist link its own free storage carries");
            initialized = true;
        }

        [[nodiscard]] bool ready() const { return initialized; }
        [[nodiscard]] size_t blockStride() const { return stride; }

        // ─── Allocate ──────────────────────────────────────────────────────
        //
        // **The caller holds the RCU domain-management lock.** Not this pool's
        // choice: `reserveStaticBufferImpl` is explicit that "serialization where
        // it is needed comes from the caller: the RCU domain-management lock
        // (DEC-050) serializes the same-entry check-then-install race between
        // concurrent reservations". Every reserver must therefore share one lock,
        // and that is the one. Taking a second lock here would buy nothing and
        // add an ordering question.
        //
        // Preference order, and the LAST step is the one that took a test to get
        // right:
        //
        //   1. a free block already on `preferred`;
        //   2. a fresh page carved on `preferred`;
        //   3. only then, a free block on any other domain.
        //
        // The obvious ordering puts the cross-domain scan second, on the
        // reasoning that reusing a remote block beats burning window. That is
        // wrong, and a NUMA placement test caught it: because a carve yields
        // several blocks at once, the very first request for a second domain
        // would find the first domain's spares and take one — so a machine would
        // never carve on its second domain at all, and every address space after
        // the first page would be remotely placed. It is not even a saving:
        // the window gets carved for that domain eventually regardless.
        //
        // Cross-domain reuse is therefore a genuine last resort, taken only when
        // the alternative is failing the allocation — at which point
        // remote-but-present clearly beats ENOMEM, since the window is a bounded
        // 1 GiB region with no free path.
        //
        // Returns a null block on window exhaustion. Never panics: this path is
        // reachable from address-space creation, which untrusted userspace
        // drives, and it must surface ENOMEM.
        [[nodiscard]] PinnedBlock allocateLocked(numa::DomainID preferred) {
            assert(initialized, "PinnedBlockPool: allocate before init");
            const size_t p = index(preferred);

            if (FreeNode* n = pop(p)) return {n, preferred};

            if (carveLocked(preferred)) {
                FreeNode* n = pop(p);
                assert(n != nullptr, "PinnedBlockPool: a successful carve yielded no block");
                return {n, preferred};
            }

            for (size_t d = 0; d < numa::kMaxDomains; d++) {
                if (d == p) continue;
                if (FreeNode* n = pop(d)) {
                    return {n, numa::DomainID{static_cast<uint16_t>(d)}};
                }
            }
            return {};
        }

        // The domain must be the one `allocateLocked` REPORTED, not the one that
        // was asked for — a block's placement is fixed at the moment its page was
        // reserved and never changes, so filing it anywhere else quietly
        // corrupts the pool's idea of where its memory is.
        void freeLocked(PinnedBlock b) {
            assert(initialized, "PinnedBlockPool: free before init");
            assert(b.ptr != nullptr, "PinnedBlockPool: free of no block");
            assert(reinterpret_cast<uintptr_t>(b.ptr) % arch::CACHE_LINE_SIZE == 0,
                   "PinnedBlockPool: freeing a pointer this pool did not hand out");
            push(index(b.domain), static_cast<FreeNode*>(b.ptr));
        }

        // ─── Instrumentation ───────────────────────────────────────────────
        //
        // The window is the scarce resource — bounded and unreclaimable — so how
        // much of it a pool has consumed is the number worth being able to read.
        // `blocksPerPage() > 1` is the whole point of this layer existing.
        [[nodiscard]] size_t pagesReserved() const { return pages; }
        [[nodiscard]] size_t blocksCarved()  const { return carved; }
        [[nodiscard]] size_t blocksPerPage() const {
            return stride > arch::smallPageSize ? 1 : arch::smallPageSize / stride;
        }

    private:
        // The intrusive link, living in the block's own bytes. Free storage is
        // the pool's to use; the block is the caller's only while allocated.
        struct FreeNode { FreeNode* next; };

        static constexpr size_t roundUpTo(size_t v, size_t a) {
            return (v + a - 1) & ~(a - 1);
        }

        static size_t index(numa::DomainID d) {
            const size_t i = static_cast<size_t>(d.value);
            assert(i < numa::kMaxDomains, "PinnedBlockPool: domain id out of range");
            return i;
        }

        [[nodiscard]] FreeNode* pop(size_t d) {
            FreeNode* n = free_[d];
            if (n != nullptr) free_[d] = n->next;
            return n;
        }

        void push(size_t d, FreeNode* n) {
            n->next  = free_[d];
            free_[d] = n;
        }

        // One reservation, sliced into as many blocks as it holds. The
        // reservation is page-granular whatever we ask for, so we ask for the
        // whole page and use all of it — which is the entire saving this layer
        // provides over each consumer reserving its own.
        [[nodiscard]] bool carveLocked(numa::DomainID d) {
            const size_t bytes = roundUpTo(stride, arch::smallPageSize);
            void* raw = VMSubstrate::tryReservePerDomainStaticBuffer(bytes, d);
            if (raw == nullptr) return false;

            const size_t count = bytes / stride;
            auto* base = static_cast<unsigned char*>(raw);
            // Pushed back to front so the first block handed out is the lowest
            // address — not required, but it makes a pool's allocation order
            // readable in a dump instead of reversed.
            for (size_t i = count; i-- > 0;) {
                push(index(d), reinterpret_cast<FreeNode*>(base + i * stride));
            }
            pages  += bytes / arch::smallPageSize;
            carved += count;
            return true;
        }

        FreeNode* free_[numa::kMaxDomains] = {};
        size_t    stride      = 0;
        size_t    pages       = 0;
        size_t    carved      = 0;
        bool      initialized = false;
    };

}  // namespace kernel::mm

#endif  // CROCOS_PINNED_BLOCK_POOL_H
