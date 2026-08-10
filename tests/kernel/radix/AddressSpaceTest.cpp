//
// radix-tree Phase 3 — the control block, creation and teardown
// (§7.4; DEC-082 / DEC-101).
//
// DEC-101 is a SEQUENCE with an explicit reverse unwind, and the phase plan is
// blunt about §7.4: **every reordering reviewed was a fatal**. So the tests are
// shaped around the orderings rather than around "creating an address space
// works", and the sharpest of them is the unwind:
//
//   "Any null mid-sequence unwinds in reverse — free the pools drawn so far,
//    free the root page, **`deinit()`** ... return the control block to its
//    freelist — and surfaces `ENOMEM`."
//
// The `deinit()` is the step a reader forgets, and forgetting it "leaks the
// pinned reservation permanently and silently, and breaks vmsmalloc DEC-050's
// high-water-mark claim, since the reservation would then grow with cumulative
// process churn". Nothing observable goes wrong at the time. The sweep below
// fails creation at every allocation index in turn and requires the accounting
// to come back to baseline each time — which is the only way that step's absence
// is visible.
//
// The other thing worth stating: **a recycled block is the caller's to re-zero**
// (vmsmalloc DEC-051). Zero-fill is a per-RESERVATION guarantee, so a block
// coming off the freelist carries its previous tenant's `dying` flag. A stale
// one means the next address space is born dead — silent, durable, and
// impossible to attribute later.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"

#include <mem/radix/AddressSpace.h>
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>

#include <cstdio>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA    = rdx::HarnessSlotCodec<GA>;
using BlockA    = rdx::ControlBlock<GA, CodecA>;
using StoreA    = rdx::ControlBlockStore<GA, CodecA>;
using TreeA     = rdx::CoreTree<GA, CodecA>;
using ValidA    = TreeValidator<GA, CodecA>;

constexpr uint64_t kPage = 4096;

// The real ceiling, not a convenient small number. `CoreTree::bindToBucket`
// asserts the pool is at least this deep — correctly, since a shallower one
// turns every operation into a shortfall-and-barrier round trip — so a test that
// wanted a cheaper sweep would have to weaken a load-bearing assert to get it.
// The sweep is ~230 creations of ~230 allocations each and runs in well under a
// second, so there was nothing to buy.
constexpr unsigned kTestRecords = rdx::deferredReleaseBound(GA);

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// The bare arena, without RadixHarness's own domain and pools — an address space
// creates those itself, and having the fixture create a second set would make
// every accounting assertion below ambiguous.
struct BareArena {
    explicit BareArena(size_t cpus = 1, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &allToDomainZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        kernel::mm::radix::HarnessSlotBase::bind(
            static_cast<uintptr_t>(VS::arenaVirtualBase(0).value), kMockArenaBytes);
        resetAccounting();
        resetInjection();
    }
    ~BareArena() {
        kernel::rcu::test::resetDomainManagementState();
        resetInjection();
        VS::test::shutdown();
    }
};

}  // namespace

// ─── The block anchors what §7.4 says it anchors ───────────────────────────

TEST(radix_address_space_creation_publishes_a_usable_block) {
    BareArena arena;
    StoreA    store;
    BlockA* block = nullptr;

    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);
    ASSERT_TRUE(block != nullptr);

    // Everything §7.4 names as anchored here, live and usable.
    ASSERT_TRUE(block->domain.initialized());
    ASSERT_TRUE(block->clusters.valid());
    ASSERT_TRUE(block->pools.perCpu == kTestRecords);
    ASSERT_TRUE(!rdx::isDying(*block));
    ASSERT_TRUE(block->generation != 0);

    // ...and the address space actually works.
    ASSERT_TRUE(block->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = block->clusters.treeFor(0);
    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(static_cast<bool>(t.lookup(0)));

    rdx::destroyAddressSpace(store, block);
    assertNoLiveObjects("creation and teardown");
}

// ─── Teardown returns everything ───────────────────────────────────────────

TEST(radix_address_space_teardown_returns_to_baseline) {
    BareArena arena;
    StoreA    store;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    // Populate several clusters, including one that grows, and then churn — so
    // the teardown has nodes, mappings, an outstanding DeferredRelease or two,
    // and more than one bucket to walk.
    const uint64_t zone = rdx::slotSpan(GA, 0);
    for (uint64_t b = 0; b < 3; b++) {
        const uint64_t va = b * zone + b * kPage;
        ASSERT_TRUE(block->clusters.ensureCovers(va, va + kPage - 1)
                    == rdx::ClusterStatus::Ok);
        TreeA t = block->clusters.treeFor(rdx::bucketIndexFor<GA>(va));
        for (unsigned k = 0; k < 4; k++) {
            auto* m = makeMapping(va);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);
        }
    }

    // Grow one of them, so teardown walks a multi-level cluster.
    ASSERT_TRUE(block->clusters.growToCover(0, rdx::nodeSpan(GA, GA.defaultRootLevel))
                == rdx::ClusterStatus::Ok);

    rdx::destroyAddressSpace(store, block);

    // Nodes, Mappings, DeferredRelease records, the bucket page — all of it. The
    // record pools in particular are NOT retire subjects, so a literal "retire
    // everything" teardown leaks them and this is where that shows.
    assertNoLiveObjects("teardown");
}

// ─── The unwind, at every step (DEC-101) ───────────────────────────────────

// Phase 0 built the injection hooks for exactly this. The sweep fails the n'th
// allocation of the creation sequence, for every n the sequence reaches, and
// requires each unwind to leave the accounting at baseline.
//
// A creation that forgot `deinit()` passes every functional check here — the
// call returns ENOMEM, nothing crashes, the next creation works — and fails only
// because the domain's pinned block never came back. That is the whole point of
// sweeping rather than testing one failure.
TEST(radix_address_space_creation_unwinds_at_every_step) {
    BareArena arena;
    StoreA    store;

    // How many allocations a full creation makes, measured rather than assumed:
    // the sequence's shape is what is under test, so hard-coding the count would
    // make the sweep stop tracking it.
    size_t fullCount = 0;
    {
        resetInjection();
        injectFailuresAfter(1u << 30);   // effectively never
        BlockA* block = nullptr;
        ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                            kTestRecords, block)
                    == rdx::CreateStatus::Ok);
        fullCount = injectionObservedCalls();
        rdx::destroyAddressSpace(store, block);
        resetInjection();
        assertNoLiveObjects("sweep baseline");
    }
    ASSERT_TRUE(fullCount > 1);

    for (size_t n = 0; n < fullCount; n++) {
        resetInjection();
        injectFailureAt(n);

        BlockA* block = nullptr;
        const auto st = rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                                kTestRecords, block);
        resetInjection();

        if (st == rdx::CreateStatus::Ok) {
            // The injected failure landed somewhere the sequence tolerates —
            // there is no such site today, but a future step with a retry would
            // make this reachable, and silently skipping it would hide it.
            ASSERT_TRUE(block != nullptr);
            rdx::destroyAddressSpace(store, block);
        } else {
            // The contract: nothing published, and nothing left behind.
            ASSERT_EQ(nullptr, block);
        }
        char what[64];
        std::snprintf(what, sizeof(what), "unwind at allocation %zu", n);
        assertNoLiveObjects(what);
    }
}

// The reservation itself failing is a different path — it happens before any
// allocation, under the domain-management lock, and its unwind is "return
// nothing, because nothing was taken".
TEST(radix_address_space_creation_handles_a_failed_reservation) {
    BareArena arena;
    StoreA    store;

    VS::test::setStaticReservationFailAt(0);
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::OutOfMemory);
    ASSERT_EQ(nullptr, block);
    VS::test::setStaticReservationFailAt(-1);

    assertNoLiveObjects("failed reservation");

    // ...and the path recovers: the next creation succeeds.
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);
    rdx::destroyAddressSpace(store, block);
    assertNoLiveObjects("recovery");
}

// ─── Recycling: the block comes back, and it comes back CLEAN ──────────────

// ─── The trailing pool array: layout, reuse, and the undersized-block guard ─
//
// The pool array moved out of `ControlBlock` and into the tail of the block's own
// reservation (D-045), which turns three implicit facts into things worth
// asserting — **especially here, because the harness cannot catch them for us**:
// the mock's static-buffer reservation is a page-granular bump pointer into one
// mmap region, so a 768-byte reservation occupies a whole 4 KiB page and an
// overrun of up to 3.3 KiB lands inside it, invisible to ASan and scribbling on
// nothing until the day it scribbles on the next block.

TEST(radix_address_space_pool_array_lies_inside_the_reservation) {
    BareArena arena(8);
    StoreA    store;
    BlockA*   block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 8,
                                        kTestRecords, block) == rdx::CreateStatus::Ok);

    const auto base    = reinterpret_cast<uintptr_t>(block);
    const auto storage = reinterpret_cast<uintptr_t>(block->poolStorage());

    // Immediately after the block, with no overlap and no gap.
    ASSERT_EQ(base + sizeof(BlockA), storage);
    // Cache-line aligned, which `create` asserts for a reason: every CPU's
    // deleters push into every CPU's head, and unaligned heads reintroduce the
    // false sharing the `alignas` exists to prevent.
    ASSERT_EQ(uintptr_t{0}, storage % arch::CACHE_LINE_SIZE);

    // Every pool the tree can address lies inside what was actually reserved.
    const uintptr_t end = base + BlockA::reservationBytes(8);
    for (unsigned c = 0; c < 8; c++) {
        const auto slot = reinterpret_cast<uintptr_t>(
            &block->pools.forCpu(static_cast<arch::ProcessorID>(c)));
        ASSERT_TRUE(slot >= storage);
        ASSERT_TRUE(slot + sizeof(rdx::DeferredReleasePool) <= end);
    }

    rdx::destroyAddressSpace(store, block);
    assertNoLiveObjects("pool array layout");
}

// The point of the whole exercise: a RECYCLED block's pools have to work, and
// they live in storage the previous tenant used. Record traffic is what proves
// it — a map/unmap cycle draws a DeferredRelease record per displaced Mapping,
// and `pools.destroy()` asserts each pool holds exactly its creation population
// at teardown, which is the detector for a record drawn and never returned.
TEST(radix_address_space_recycled_block_pools_carry_record_traffic) {
    BareArena arena;
    StoreA    store;

    const auto churn = [](BlockA& b) {
        ASSERT_TRUE(b.clusters.ensureCovers(0, (uint64_t{1} << 30) - 1)
                    == rdx::ClusterStatus::Ok);
        TreeA t = b.clusters.treeFor(0);
        for (unsigned k = 0; k < 24; k++) {
            const uint64_t va = uint64_t{k} * 64 * 1024;
            auto* m = makeMapping(va);
            ASSERT_TRUE(m != nullptr);
            ASSERT_TRUE(t.apply(va, va + kPage - 1, m) == rdx::ApplyStatus::Ok);
        }
        // Displacement: each clear defers a Mapping release through a record
        // drawn from this block's pools.
        for (unsigned k = 0; k < 24; k++) {
            const uint64_t va = uint64_t{k} * 64 * 1024;
            ASSERT_TRUE(t.apply(va, va + kPage - 1, nullptr) == rdx::ApplyStatus::Ok);
        }
        (void)kernel::rcu::drainAllQuiescent(b.domain);
    };

    BlockA* first = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, first) == rdx::CreateStatus::Ok);
    const auto* firstAddress = first;
    churn(*first);
    rdx::destroyAddressSpace(store, first);

    BlockA* second = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, second) == rdx::CreateStatus::Ok);
    // Same pinned storage, so the pools below are the previous tenant's bytes.
    ASSERT_EQ(firstAddress, second);
    ASSERT_TRUE(second->pools.perCpu == kTestRecords);
    ASSERT_TRUE(second->pools.cpuCount == 1);
    // The array is still the block's own tail, not a stale pointer carried over.
    ASSERT_EQ(reinterpret_cast<uintptr_t>(second) + sizeof(BlockA),
              reinterpret_cast<uintptr_t>(second->pools.poolsBase()));

    churn(*second);
    rdx::destroyAddressSpace(store, second);
    assertNoLiveObjects("recycled block record traffic");
}

// The guard that replaced a whole class of bug.
//
// Blocks used to be variable-sized, so a freelist handing back one whose
// trailing pool array was too short would write past the reservation — and the
// harness's page-granular bump allocator would not notice. That needed a
// size-checking scan on every take.
//
// A fixed-stride pool cannot produce an undersized block at all, so the scan is
// gone and the only remaining route is a caller asking one store for more CPUs
// than it was built for. That is caller error, and it now fails loudly at the
// boundary instead of being quietly accommodated. Unreachable in the kernel,
// where `processorCount()` is constant after boot.
TEST(radix_address_space_store_rejects_more_cpus_than_it_was_built_for) {
    BareArena arena(8);
    StoreA    store;

    BlockA* narrow = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, narrow) == rdx::CreateStatus::Ok);
    rdx::destroyAddressSpace(store, narrow);

    // The store's stride was fixed at one CPU by that first call.
    BlockA* wide = nullptr;
    EXPECT_ASSERT_FAILURE(
        (void)rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 8,
                                      kTestRecords, wide));
}

// ─── The freelist keeps each block on its own NUMA domain ──────────────────
//
// `createAddressSpace` takes a `home` domain and `tryReservePerDomainStaticBuffer`
// honours it — but only on the FIRST reservation. With one undifferentiated
// store, every reuse afterwards handed back whatever was at the head, so the
// placement was a suggestion obeyed exactly once. On a multi-socket machine that
// puts a process's control block on a remote node, and this block holds the
// generation and the pool heads every CPU reads on hot paths — which is the
// entire reason DEC-082 moved them into pinned storage.
//
// Reservations are kernel-lifetime, so a block's placement is fixed forever at
// its first reservation; the only thing the freelist can do is remember it and
// hand the block back to a tenant that wants that domain.
TEST(radix_address_space_freelist_preserves_numa_placement) {
    BareArena arena(2, 2);
    StoreA    store;

    BlockA* onZero = nullptr;
    BlockA* onOne  = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, onZero) == rdx::CreateStatus::Ok);
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{1}, 1,
                                        kTestRecords, onOne) == rdx::CreateStatus::Ok);
    ASSERT_TRUE(onZero != onOne);

    // Both go back. Order matters to the test: domain 1's block is returned
    // LAST, so a single-list freelist would hand it to the next request whatever
    // domain that request asked for.
    rdx::destroyAddressSpace(store, onZero);
    rdx::destroyAddressSpace(store, onOne);

    BlockA* wantZero = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, wantZero) == rdx::CreateStatus::Ok);
    // The block placed on domain 0, not the one most recently freed.
    ASSERT_EQ(onZero, wantZero);
    ASSERT_TRUE(wantZero->homeDomain == kernel::numa::DomainID{0});

    BlockA* wantOne = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{1}, 1,
                                        kTestRecords, wantOne) == rdx::CreateStatus::Ok);
    ASSERT_EQ(onOne, wantOne);
    ASSERT_TRUE(wantOne->homeDomain == kernel::numa::DomainID{1});

    rdx::destroyAddressSpace(store, wantZero);
    rdx::destroyAddressSpace(store, wantOne);
}

// The cross-domain fallback, tested at the condition it exists for.
//
// It is deliberately the LAST resort, not the second choice. An earlier version
// of the pool scanned other domains before carving, on the reasoning that
// reusing a remote block beats burning window — and the placement test above
// caught why that is wrong: a carve yields several blocks at once, so the very
// first request for a second domain would find the first domain's spares and
// take one, and the machine would never carve on its second domain at all.
//
// So reaching the fallback requires the window to be genuinely exhausted, which
// is what the scripted reservation failure below produces. At that point
// remote-but-present clearly beats ENOMEM.
TEST(radix_address_space_store_falls_back_across_domains_only_when_exhausted) {
    BareArena arena(2, 2);
    StoreA    store;

    BlockA* onZero = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, onZero) == rdx::CreateStatus::Ok);
    const auto* zeroAddress = onZero;
    rdx::destroyAddressSpace(store, onZero);

    // No window left. The pool cannot carve for domain 1, and RCU's slot block
    // recycles through its own freelist, so creation can still succeed — if the
    // fallback takes the block sitting on domain 0.
    VS::test::setStaticReservationFailAt(0);

    BlockA* wantOne = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{1}, 1,
                                        kTestRecords, wantOne) == rdx::CreateStatus::Ok);
    ASSERT_EQ(zeroAddress, wantOne);
    // ...and it reports where it actually IS, not where it was wanted. A caller
    // that believed the request would misplace every later decision made from it.
    ASSERT_TRUE(wantOne->homeDomain == kernel::numa::DomainID{0});

    VS::test::setStaticReservationFailAt(-1);
    rdx::destroyAddressSpace(store, wantOne);
}

// Sub-page packing — the reason this layer exists at all. A control block for
// 8 CPUs is 768 B against a 4 KiB page, so five address spaces should share one
// page of the window instead of taking five.
TEST(radix_address_space_store_packs_blocks_into_a_page) {
    BareArena arena(8);
    StoreA    store;

    BlockA* blocks[5] = {};
    for (auto& b : blocks) {
        ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 8,
                                            kTestRecords, b) == rdx::CreateStatus::Ok);
    }

    // Five per page at a 768 B stride. This asserted FOUR while ITEM-084's
    // reserve was in the trailing array: one extra cache-line-aligned
    // `DeferredReleasePool` took the stride to 832 B and 4,096/832 is 4. D-059
    // deleted the reserve — the record budget makes a per-address-space ceiling
    // unnecessary rather than relocating it — so the stride and this number both
    // go back, and the per-CPU population stays at 32 rather than 230.
    ASSERT_TRUE(store.pool.blocksPerPage() >= 5);
    // One page for five blocks, where the old whole-reservation-per-block scheme
    // took five.
    ASSERT_EQ(size_t{1}, store.pool.pagesReserved());

    // Distinct, and each cache-line aligned so two address spaces never share a
    // line — these blocks hold the generation and pool heads every CPU reads.
    for (unsigned i = 0; i < 5; i++) {
        ASSERT_EQ(uintptr_t{0},
                  reinterpret_cast<uintptr_t>(blocks[i]) % arch::CACHE_LINE_SIZE);
        for (unsigned j = i + 1; j < 5; j++) ASSERT_TRUE(blocks[i] != blocks[j]);
    }

    for (auto& b : blocks) rdx::destroyAddressSpace(store, b);
    assertNoLiveObjects("sub-page packing");
}

// ─── The static-buffer ceiling, DERIVED (R-16) ─────────────────────────────
//
// How many concurrent address spaces the pinned static-buffer window can hold.
// `VMSubstrate.h` used to state the answer in prose, and **it was wrong twice** —
// once when D-051 moved the root bucket page into its own pool, and again when
// D-059 deleted the reserve slot from the control block's trailing array. Both
// times the code that changed was three files away from the sentence that
// described it.
//
// So the number is computed here from the live pools instead. DEC-093's rule for
// the geometry ("retuning costs a descriptor edit and re-derived figures, and a
// transcribed number would silently survive a retune that changed it") is exactly
// the rule this figure needed and did not have.
//
// **The metric is each consumer's SHARE OF A PAGE, not its stride**, and the
// difference is the whole reason the old arithmetic drifted. A 768 B block does not
// cost 768 B of window: five fit in a 4,096 B page and the sixth does not, so each
// costs 4,096/5 = 819.2 B. The RCU veneer already reports its own cost this way
// (`RCU.cpp`'s `windowBytes` is `smallPageSize / blocksPerPage()`), and this reuses
// that definition so the two agree.
TEST(radix_static_buffer_ceiling_is_derived_from_the_live_pools) {
    constexpr size_t kCpus = 8;                       // the consumer-desktop target
    BareArena arena(kCpus);
    StoreA    store;

    // Draw one of everything, because every pool is inert until its first
    // allocation — `blocksPerPage()` reads zero before then, and a ceiling derived
    // from zeros would be silently infinite.
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, kCpus,
                                        rdx::kDefaultRecordsPerCpu, block)
                == rdx::CreateStatus::Ok);

    // Per-address-space share of a page, one row per pinned consumer.
    const size_t controlBlockShare = kPage / store.pool.blocksPerPage();
    const size_t rootPageShare     = kPage / store.rootPages.blocksPerPage();
    const size_t rcuSlotShare      = kPage / rcu::test::slotPoolBlocksPerPage();
    const size_t perAddressSpace   = controlBlockShare + rootPageShare + rcuSlotShare;

    // ─── The one number not derived here, and why that is acceptable ────
    //
    // `kernel::mm::getKernelMemRegionSize()` is the authority, and its header
    // cannot be included in the harness: `kmemlayout.h` needs the real
    // `arch::PageTable` template and `arch::pageTableDescriptor`, which the mocks
    // do not provide. So the window size is mirrored, and the mirror is a
    // deliberate asymmetry rather than an oversight —
    //
    //   * the quantity that has DRIFTED, twice, is `perAddressSpace`: it is the
    //     sum of three pool strides that change whenever a struct gains a field,
    //     and it is derived above from the live pools with nothing transcribed;
    //   * the window size is one constexpr in one header, derived from the page
    //     table's own geometry, and has been 1 GiB since the layout existed.
    //
    // Mirroring the stable term to derive the volatile one is the trade this test
    // exists to make. It is NOT the 64 MiB mock arena: the ceiling is a claim
    // about production, and using the fixture's arena would silently answer a
    // different question.
    constexpr size_t regionBytes = size_t{1} << 30;      // getKernelMemRegionSize()
    const size_t ceiling = regionBytes / perAddressSpace;

    std::printf("\n  static-buffer window: %zu concurrent address spaces at %zu CPUs\n",
                ceiling, kCpus);
    std::printf("      consumer            | stride | blocks/page | B per address space\n");
    std::printf("      radix control block | %6zu | %11zu | %zu\n",
                store.pool.blockStride(), store.pool.blocksPerPage(), controlBlockShare);
    std::printf("      radix root bucket   | %6zu | %11zu | %zu\n",
                store.rootPages.blockStride(), store.rootPages.blocksPerPage(), rootPageShare);
    std::printf("      RCU slot array      | %6s | %11zu | %zu\n",
                "-", rcu::test::slotPoolBlocksPerPage(), rcuSlotShare);
    std::printf("      total               |        |             | %zu   (region %zu MiB)\n\n",
                perAddressSpace, regionBytes / (1024 * 1024));

    // Each consumer must actually be PACKING. A row that fell back to one block
    // per page is the regression this layer exists to prevent, and it would show up
    // as a quietly smaller ceiling rather than as a failure anywhere else.
    ASSERT_TRUE(store.pool.blocksPerPage() > 1);
    ASSERT_TRUE(rcu::test::slotPoolBlocksPerPage() > 1);
    // The root bucket page is the one that legitimately does not pack: a
    // `BucketTable` is exactly a page by design (D-051), which is why it got its
    // own pool instead of being folded into the control block's stride.
    ASSERT_EQ(size_t{1}, store.rootPages.blocksPerPage());
    ASSERT_EQ(kPage, rootPageShare);

    // The root page dominates — it is a whole page against everything else's
    // fractions — so it is the row to attack if this ceiling ever needs to move.
    // Asserted rather than remarked, because it is the actionable conclusion.
    ASSERT_TRUE(rootPageShare > controlBlockShare + rcuSlotShare);

    // ─── Exact, per row, and that is the point ──────────────────────────────
    //
    // A bracket was tried first and it was not a detector: re-adding eight pool
    // slots to the control block's trailing array — the same shape of change
    // ITEM-084 made, which is what put the wrong number in `VMSubstrate.h` — moved
    // the ceiling 180,795 -> 165,573 and a `> 150000 && < 200000` bracket passed
    // it. A 9% window regression that no test objects to is how this figure went
    // stale twice.
    //
    // So each row is exact. That means an intentional change to any of these
    // structs fails here and has to be re-derived deliberately — which is the
    // forcing function DEC-093 requires of the geometry's figures and that this
    // one never had. The printf above hands you the new numbers, so honouring it
    // costs one edit, and the per-row form says WHICH consumer moved.
    ASSERT_EQ(size_t{819},  controlBlockShare);       // 768 B stride, 5 per page
    ASSERT_EQ(size_t{4096}, rootPageShare);           // one whole page, by design
    ASSERT_EQ(size_t{1024}, rcuSlotShare);            // 8 x 128 B slots, 4 per page
    ASSERT_EQ(size_t{5939}, perAddressSpace);
    ASSERT_EQ(size_t{180795}, ceiling);

    rdx::destroyAddressSpace(store, block);
    assertNoLiveObjects("static-buffer ceiling");
}

TEST(radix_address_space_blocks_recycle_through_the_store) {
    BareArena arena;
    StoreA    store;

    BlockA* first = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, first)
                == rdx::CreateStatus::Ok);
    const uint64_t gen1 = first->generation;

    // Mark it dying and tear it down, so the block goes back to the freelist
    // carrying a set flag — which is precisely the state a recycled block must
    // not inherit.
    rdx::destroyAddressSpace(store, first);

    BlockA* second = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, second)
                == rdx::CreateStatus::Ok);

    // The same pinned storage: reservations are kernel-lifetime and never
    // returned to VMSubstrate, which is what keeps the high-water mark at
    // "maximum concurrent address spaces" rather than growing with churn.
    ASSERT_EQ(first, second);

    // Re-zeroed. A stale `dying` would make this address space born dead:
    // every operation would take the teardown path, silently, forever.
    ASSERT_TRUE(!rdx::isDying(*second));
    // ...and the generation MOVED, which is what lets Phase 4's descent cache
    // tell a recycled block's entries from a live one's.
    ASSERT_TRUE(second->generation != gen1);

    // It works.
    ASSERT_TRUE(second->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = second->clusters.treeFor(0);
    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, m) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(static_cast<bool>(t.lookup(0)));

    rdx::destroyAddressSpace(store, second);
    assertNoLiveObjects("recycling");
}

// ─── The dying flag ────────────────────────────────────────────────────────

TEST(radix_address_space_dying_flag_is_set_before_the_barrier) {
    BareArena arena;
    StoreA    store;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    ASSERT_TRUE(!rdx::isDying(*block));
    // Set it by hand and observe it through the acquire side — the ordering pair
    // is named in §6.6 precisely because the flag alone establishes nothing and
    // a bare spelling would invite someone to trust it on its own.
    block->dying.store(1, rdx::kDyingFlagStore);
    ASSERT_TRUE(rdx::isDying(*block));
    block->dying.store(0, rdx::kPrivateInit);

    rdx::destroyAddressSpace(store, block);
    assertNoLiveObjects("dying flag");
}

// ─── DEC-100's unit-decomposed walk ────────────────────────────────────────

// The three properties that distinguish it from a synchronous post-order
// release, driven by running §7.4's steps by hand so the window between the walk
// and the drain is observable at all.
//
// The one that matters most for Phase 4 is the **marking**. §7.4: "Without any
// marking at all, teardown would be the only unlink path setting no mark,
// leaving a foreign CPU's surviving cache entry pointed at a node that is
// unlinked, unmarked, alive, and holding slots that point at freed children."
// There is no descent cache yet, so nothing observes it today — which is exactly
// why it needs a test now rather than when the cache lands and the symptom is a
// silent wrong mapping.
TEST(radix_teardown_walk_marks_and_retires_rather_than_destroying) {
    BareArena arena;
    StoreA    store;
    BlockA* block = nullptr;
    ASSERT_TRUE(rdx::createAddressSpace(store, kernel::numa::DomainID{0}, 1,
                                        kTestRecords, block)
                == rdx::CreateStatus::Ok);

    ASSERT_TRUE(block->clusters.ensureCovers(0, kPage - 1) == rdx::ClusterStatus::Ok);
    TreeA t = block->clusters.treeFor(0);
    // Two disjoint sub-ranges, so the cluster is a root plus a real child and
    // the walk has more than one unit to run.
    auto* a = makeMapping(0);
    auto* b = makeMapping(4 * kPage);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(t.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(t.apply(4 * kPage, 5 * kPage - 1, b) == rdx::ApplyStatus::Ok);
    (void)kernel::rcu::drainAllQuiescent(block->domain);

    const size_t nodesBefore = t.nodeCount();
    ASSERT_TRUE(nodesBefore > 1);
    rdx::NodeRef root = t.root();

    // Capturing a node pointer across the walk is legal for the test for the
    // same reason the walk itself carries one across its children's sections:
    // by here nothing else is running.
    ASSERT_TRUE(!rdx::state::isMarked(root.stateWord().load(RELAXED)));

    // §7.4's steps, by hand, so the window is observable.
    block->dying.store(1, rdx::kDyingFlagStore);
    kernel::rcu::synchronize(block->domain);

    const size_t liveNodesBeforeWalk = liveCountOf<rdx::Node<GA, 32>>()
                                     + liveCountOf<rdx::Node<GA, 16>>();
    block->clusters.tearDownClusters();

    // 1. MARKED. The property with no symptom until Phase 4.
    ASSERT_TRUE(rdx::state::isMarked(root.stateWord().load(RELAXED)));

    // 2. RETIRED, not destroyed: the nodes are still live objects between the
    //    walk and the drain. A synchronous walk destroys them in step, and this
    //    count would already be zero.
    ASSERT_EQ(liveNodesBeforeWalk,
              liveCountOf<rdx::Node<GA, 32>>() + liveCountOf<rdx::Node<GA, 16>>());

    // 3. The bucket word is cleared by the walk's final unit, inside its own
    //    section — so the cluster is unreachable even though its nodes are not
    //    yet destroyed.
    ASSERT_TRUE(!block->clusters.bucketIsOccupied(0));

    // ...and the drain is what actually destroys them.
    (void)kernel::rcu::drainAllQuiescent(block->domain);
    ASSERT_EQ(size_t{0}, liveCountOf<rdx::Node<GA, 32>>() + liveCountOf<rdx::Node<GA, 16>>());

    {
        kernel::rcu::DomainManagementLockGuard guard;
        block->clusters.freeRootPageLocked();
    }
    block->pools.destroy();
    (void)block->domain.deinit();
    {
        kernel::rcu::DomainManagementLockGuard guard;
        store.returnLocked(block);
    }
    assertNoLiveObjects("unit-decomposed walk");
}
