//
// vmsmalloc Phase 8 — mmap-backed MockVMSubstrate (page primitives + setup).
//
// Lays out a single mmap'd VA region as:
//   [guard page] [per-domain static buffers] [per-CPU CpuLocal pages] [pool]
// The guard page keeps offset 0 (== vmsBase) out of the allocatable range, so
// the DEC-015 empty-stack marker (offset 0 -> nullptr) can never collide with a
// real descriptor — the same property the kernel gets from arena 0's metadata
// prefix. allocPage/freePage serve the pool under a mutex (multiple harness
// threads model multiple CPUs and allocate concurrently).
//

#include <mem/VMSubstrate.h>      // mock header (include-path ordering)
#include <VMSubstrateSlab.h>      // real: perDomainBufs, vmsmallocLateInit, kPerDomainBufBytes
#include <CpuLocal.h>             // real: kCpuLocalBytes
#include <arch.h>

#include <asan_poison.h>            // radix-tree DEC-052 oracle interaction

#include <sys/mman.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace kernel::mm::vmsmalloc {
    // Defined here for the harness (the kernel build defines it in
    // VMSubstrateSlab.cpp, which the harness does not compile).
    void* perDomainBufs[kMaxDomains] = {};
}

namespace kernel::mm::VMSubstrate {

namespace {
    constexpr size_t kRegionBytes = size_t{64} * 1024 * 1024;   // 64 MiB (P8-DEC-001)
    const size_t     kPageSize    = arch::smallPageSize;

    // Headroom in the static-buffer region above vmsmalloc's own per-domain
    // buffers, for other consumers of reservePerDomainStaticBuffer. RCU Phase 2
    // is the first: kernel::rcu::Domain::init reserves one page per 32 CPUs per
    // domain constructed, and the RCU harness builds several domains per test.
    // Without this the bump pointer runs past gStaticEnd and MockVMSubstrate
    // aborts the whole runner rather than failing a test.
    constexpr size_t kExtraStaticPages = 256;

    std::mutex        gMutex;
    uint8_t*          gRegion       = nullptr;   // mmap base (== vmsBase)
    size_t            gRegionBytes  = 0;
    uint8_t*          gStaticNext   = nullptr;   // bump pointer for static buffers
    uint8_t*          gStaticEnd    = nullptr;
    uint8_t*          gCpuLocalBase = nullptr;
    // Intrusive free list over the allocatable pool: each free page's first
    // word holds the next free page (no heap allocation -> no interaction with
    // the harness's per-test leak tracker).
    void*             gFreeHead     = nullptr;
    size_t            gActiveCount  = 0;
}

// The page primitives bracket every poisoned region the DEC-052 oracle creates,
// and both of them WRITE into the page — allocPage memsets it, freePage
// threads it onto the intrusive free list through its first word. A page whose
// slab still carries poisoned (destroyed) slots would therefore trip ASan
// inside the mock itself, reported against the harness rather than against the
// defect under test. Unpoisoning the whole page at each transition is the fix,
// and it is also semantically right: page ownership has left the slab, so no
// object-level poison on it means anything any more.
//
// Costs nothing when no oracle is active (the calls compile away without ASan,
// and unpoisoning unpoisoned memory is a no-op), which is why this lives in the
// shared mock rather than in a radix-only fork of it.
static void unpoisonWholePage(void* p) {
    CroCOSTest::unpoisonRegion(p, kPageSize);
}

void* allocPage() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gFreeHead) {
        std::fprintf(stderr, "MockVMSubstrate: out of pool pages\n");
        std::abort();
    }
    void* p = gFreeHead;
    // Unpoison BEFORE reading the free-list link out of the page, not after.
    //
    // The order looks arbitrary and is not. The DEC-052 oracle poisons
    // `sizeof(T)` bytes at `destroy<T>` — deliberately AFTER `vmsfree`, so the
    // allocator's own validation and its DEC-024 scribble do not run over
    // poisoned memory. For a slab-backed T that poison covers one slot. For a
    // **whole-page** T (radix's 4 KiB BucketTable is the first) it covers the
    // entire page, INCLUDING the word this function threads the free list
    // through — so the page returns to the pool fully poisoned and the pop below
    // reads a poisoned link.
    //
    // Latent until a whole-page object existed, which is why it surfaced as a
    // use-after-poison inside allocPage the first time an address space was
    // created and destroyed.
    unpoisonWholePage(p);
    gFreeHead = *reinterpret_cast<void**>(p);
    gActiveCount++;
    // Per DEC-046 the kernel's allocPage invalidates the caller's stale TLB
    // entry; in userspace we instead zero so a re-handed-out page never carries
    // a previous descriptor's bytes (cheap and keeps tests deterministic).
    std::memset(p, 0, kPageSize);
    return p;
}

// DEC-048: the failable sibling. In the kernel this fails on arena-VA
// exhaustion, on failure to back a lazily-installed page-table/occupancy page,
// and on physical-page exhaustion; here the single pool stands in for all
// three. The scripted counter exists because draining a 64 MiB pool to reach
// the interesting path would make every failure test slow and would perturb
// the very allocator state the test is checking survived.
static long gPageAllocFailAt = -1;   // -1 == never fail
static long gTryPageAllocCalls = 0;

void* tryAllocPage() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const long n = gTryPageAllocCalls++;
        if (gPageAllocFailAt >= 0 && n >= gPageAllocFailAt) return nullptr;
        if (!gFreeHead) return nullptr;
    }
    return allocPage();
}

void freePage(void* p) {
    std::lock_guard<std::mutex> lock(gMutex);
    unpoisonWholePage(p);
    *reinterpret_cast<void**>(p) = gFreeHead;
    gFreeHead = p;
    gActiveCount--;
}

// DEC-047: in the kernel this remaps the VA read-only onto a sentinel page so a
// racing popper's speculative read stays a harmless garbage read. Userspace has
// no page tables / TLB and the Phase 8 harness does not simulate the reclaim
// race (deferred to the in-kernel stress test), so the mock just recycles the
// page exactly like freePage to keep the active-page leak accounting correct.
void reclaimSlabPage(void* p) {
    freePage(p);
}

void* mapMMIOPage(phys_addr) { std::abort(); }

// Scripted failure for the try-variant, so a consumer's creation-unwind path is
// drivable without exhausting a 64 MiB arena first. -1 == never fail.
static long gStaticReservationFailAt = -1;
static long gStaticReservationCalls  = 0;

void* tryReservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const long n = gStaticReservationCalls++;
        if (gStaticReservationFailAt >= 0 && n >= gStaticReservationFailAt) return nullptr;
        const size_t pages = (byteSize + kPageSize - 1) / kPageSize;
        if (gStaticNext + pages * kPageSize > gStaticEnd) return nullptr;
    }
    return reservePerDomainStaticBuffer(byteSize, d);
}

namespace test {
    void setStaticReservationFailAt(long n) {
        std::lock_guard<std::mutex> lock(gMutex);
        gStaticReservationFailAt = n;
        gStaticReservationCalls  = 0;
    }
    void setPageAllocFailAt(long n) {
        std::lock_guard<std::mutex> lock(gMutex);
        gPageAllocFailAt   = n;
        gTryPageAllocCalls = 0;
    }
}

void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID) {
    std::lock_guard<std::mutex> lock(gMutex);
    // Rounded to whole pages, exactly like the kernel's implementation
    // (VMSubstrate.cpp:982). Two consumers depend on it: every reservation comes
    // back page-aligned (RCU's slot array is alignas(64) and would otherwise be
    // misaligned behind an unrounded vmsmalloc buffer), and consecutive
    // reservations stay exactly pageSize apart, which is the contiguity property
    // kernel::rcu::Domain::init checks for.
    const size_t pages = (byteSize + kPageSize - 1) / kPageSize;
    uint8_t* p = gStaticNext;
    gStaticNext += pages * kPageSize;
    if (gStaticNext > gStaticEnd) { std::abort(); }
    std::memset(p, 0, pages * kPageSize);   // zero-fill contract
    return p;
}

void* cpuLocalPageFor(arch::ProcessorID i) {
    return gCpuLocalBase + static_cast<size_t>(i) * kCpuLocalBytes;
}

virt_addr arenaVirtualBase(size_t index) {
    return virt_addr{reinterpret_cast<uint64_t>(gRegion) + index * kPageSize};
}

namespace test {

void initialize(size_t cpuCount, size_t domainCount) {
    void* base = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { std::abort(); }
    gRegion      = static_cast<uint8_t*>(base);
    gRegionBytes = kRegionBytes;

    // Layout: guard page, then static-buffer region, then CpuLocal pages, then pool.
    uint8_t* p = gRegion + kPageSize;                       // skip guard page (offset 0)
    const size_t staticBytes =
        domainCount * vmsmalloc::kPerDomainBufBytes + kExtraStaticPages * kPageSize;
    gStaticNext  = p;
    gStaticEnd   = p + staticBytes;
    p += staticBytes;

    gCpuLocalBase  = p;
    p += cpuCount * kCpuLocalBytes;
    std::memset(gCpuLocalBase, 0, cpuCount * kCpuLocalBytes);   // zero CpuLocal pages

    // Remaining whole pages form the allocatable pool; link them into the
    // intrusive free list (highest page becomes the tail).
    uint8_t* poolBase = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(p) + kPageSize - 1) & ~(kPageSize - 1));
    uint8_t* poolEnd  = gRegion + gRegionBytes;
    gFreeHead = nullptr;
    gActiveCount = 0;
    for (uint8_t* q = poolBase; q + kPageSize <= poolEnd; q += kPageSize) {
        *reinterpret_cast<void**>(q) = gFreeHead;
        gFreeHead = q;
    }

    // Reserve and zero the per-domain buffers, then construct the per-(domain,
    // class) ChainedTreiberStacks (mirrors the kernel's vmsmallocInit ->
    // vmsmallocLateInit flow).
    for (size_t d = 0; d < domainCount; d++) {
        vmsmalloc::perDomainBufs[d] =
            reservePerDomainStaticBuffer(vmsmalloc::kPerDomainBufBytes, numa::DomainID{static_cast<uint16_t>(d)});
    }
    vmsmalloc::vmsmallocLateInit(reinterpret_cast<uintptr_t>(gRegion), gRegionBytes);
}

void shutdown() {
    if (gRegion) { munmap(gRegion, gRegionBytes); }
    gRegion = nullptr; gRegionBytes = 0;
    gStaticNext = gStaticEnd = gCpuLocalBase = nullptr;
    gStaticReservationFailAt = -1;
    gStaticReservationCalls  = 0;
    gFreeHead = nullptr; gActiveCount = 0;
    for (auto& b : vmsmalloc::perDomainBufs) b = nullptr;
}

size_t activePageCount() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gActiveCount;
}

} // namespace test
} // namespace kernel::mm::VMSubstrate
