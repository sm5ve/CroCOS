// PageAllocatorStress.cpp
// Indefinite multi-threaded stress test for PageAllocatorImpl.
// Prints performance statistics every ~5 seconds.
//
// Usage:
//   ./PageAllocatorStress [options]
//
//   --domains  N       NUMA domains (default 2)
//   --pages    N       Big pages per domain (default 128)
//   --threads  N       Worker threads per domain (default 4)
//   --batch    N       Max pages per alloc call (default 64)
//   --interval N       Report interval in milliseconds (default 5000)
//
// Ctrl+C to stop gracefully.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cassert>

#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <optional>
#include <string>

#include <mem/PageAllocator.h>
#include <mem/mm.h>
#include <mem/NUMA.h>
#include <arch.h>

using namespace kernel::mm;
namespace PA = kernel::mm::PageAllocator;

// ============================================================================
// Forward declaration of mock helper exposed by StressMocks.cpp
// ============================================================================
namespace arch { void stressSetProcessorCount(size_t n); }

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    size_t numDomains        = 2;
    size_t bigPagesPerDomain = 128;
    size_t threadsPerDomain  = 4;
    size_t maxBatch          = 64;
    size_t reportIntervalMs  = 5000;
};

static Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--domains")  == 0) cfg.numDomains        = atoi(argv[++i]);
        if (strcmp(argv[i], "--pages")    == 0) cfg.bigPagesPerDomain = atoi(argv[++i]);
        if (strcmp(argv[i], "--threads")  == 0) cfg.threadsPerDomain  = atoi(argv[++i]);
        if (strcmp(argv[i], "--batch")    == 0) cfg.maxBatch          = atoi(argv[++i]);
        if (strcmp(argv[i], "--interval") == 0) cfg.reportIntervalMs  = atoi(argv[++i]);
    }
    return cfg;
}

// ============================================================================
// Statistics — one per worker thread, cache-line padded to avoid false sharing
// ============================================================================

struct alignas(64) ThreadStats {
    std::atomic<uint64_t> allocCalls    {0};
    std::atomic<uint64_t> freeCalls     {0};
    std::atomic<uint64_t> pagesAllocated{0};
    std::atomic<uint64_t> oomEvents     {0};
    char _pad[64 - 4 * sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(ThreadStats) == 64, "ThreadStats must be exactly one cache line");

// ============================================================================
// Allocator setup helpers (mirrors TestPageAllocatorImpl from the unit tests)
// ============================================================================

static constexpr uint64_t domainBase(size_t d) {
    return (d + 1) * (1ull << 30); // 1 GiB-aligned, never overlapping
}

struct BootstrapBuffer {
    std::vector<uint8_t> storage;
    explicit BootstrapBuffer(size_t bytes) : storage(bytes, 0) {}
    BootstrapAllocator makeAllocator() {
        return BootstrapAllocator(storage.data(), storage.size());
    }
};

struct StressAllocatorImpl {
    std::vector<BootstrapBuffer>           domainBuffers;
    LocalPool*                             localPools[arch::MAX_PROCESSOR_COUNT] = {};
    kernel::numa::NUMATopology             topology = kernel::numa::NUMATopology::build(
        kernel::numa::EmptyIterable<kernel::numa::ProcessorAffinityEntry>{},
        kernel::numa::EmptyIterable<kernel::numa::MemoryRangeAffinityEntry>{},
        kernel::numa::EmptyIterable<kernel::numa::GenericInitiatorEntry>{}
    );
    std::optional<kernel::numa::NUMAPolicy> policy;
    PageAllocatorImpl                      impl;

    StressAllocatorImpl(const Config& cfg) {
        const size_t numDomains       = cfg.numDomains;
        const size_t bigPages         = cfg.bigPagesPerDomain;
        const size_t threadsPerDomain = cfg.threadsPerDomain;
        const size_t totalThreads     = numDomains * threadsPerDomain;

        // Build CPU→domain affinity entries.
        std::vector<kernel::numa::ProcessorAffinityEntry> procEntries;
        procEntries.reserve(totalThreads);
        for (size_t d = 0; d < numDomains; d++) {
            for (size_t t = 0; t < threadsPerDomain; t++) {
                size_t cpu = d * threadsPerDomain + t;
                procEntries.push_back({
                    static_cast<arch::ProcessorID>(cpu),
                    static_cast<uint32_t>(d),
                    static_cast<uint32_t>(d)
                });
            }
        }
        topology = kernel::numa::NUMATopology::build(
            procEntries,
            kernel::numa::EmptyIterable<kernel::numa::MemoryRangeAffinityEntry>{},
            kernel::numa::EmptyIterable<kernel::numa::GenericInitiatorEntry>{}
        );

        const kernel::numa::NUMAPolicy* policyPtr = nullptr;
        if (numDomains > 1) {
            policy.emplace(topology);
            policyPtr = &policy.value();
        }

        domainBuffers.reserve(numDomains);
        Vector<NUMAPool*> numaPools;

        for (size_t d = 0; d < numDomains; d++) {
            kernel::numa::DomainID domainId{static_cast<uint16_t>(d)};
            Vector<phys_memory_range> ranges;
            uint64_t base = domainBase(d);
            ranges.push({ phys_addr(base), phys_addr(base + bigPages * arch::bigPageSize) });

            // Measuring pass
            BootstrapAllocator measuring;
            createNumaPool(measuring, ranges, domainId);
            for (size_t t = 0; t < threadsPerDomain; t++)
                createLocalPool(measuring, topology);

            domainBuffers.emplace_back(measuring.bytesNeeded());

            // Real pass
            BootstrapAllocator real = domainBuffers.back().makeAllocator();
            numaPools.push(createNumaPool(real, ranges, domainId));

            size_t cpuBase = d * threadsPerDomain;
            for (size_t t = 0; t < threadsPerDomain; t++)
                localPools[cpuBase + t] = createLocalPool(real, topology);
        }

        impl = createPageAllocator(move(numaPools), localPools,
                                   totalThreads, nullptr, policyPtr);
    }
};

// ============================================================================
// Stop flag (set by SIGINT)
// ============================================================================

static std::atomic<bool> g_stop{false};

static void handleSigint(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

// ============================================================================
// Number formatting helpers
// ============================================================================

static std::string fmtNum(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}

static std::string fmtRate(double r) {
    return fmtNum(static_cast<uint64_t>(r));
}

// ============================================================================
// Worker thread
// ============================================================================

static constexpr size_t HARD_MAX_BATCH = 1024;

static void workerThread(PageAllocatorImpl& impl,
                         ThreadStats& stats,
                         size_t maxBatch) {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(1, maxBatch);

    PageRef pages[HARD_MAX_BATCH];

    while (!g_stop.load(std::memory_order_relaxed)) {
        size_t count = 0;
        impl.allocatePages(pick(rng),
            [&](PageRef r) { pages[count++] = r; },
            AllocBehavior::GRACEFUL_OOM);

        stats.allocCalls.fetch_add(1, std::memory_order_relaxed);

        if (count == 0) {
            stats.oomEvents.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats.pagesAllocated.fetch_add(count, std::memory_order_relaxed);
            impl.freePages(pages, count);
            stats.freeCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ============================================================================
// Reporter thread — wakes every reportIntervalMs and prints a stats line
// ============================================================================

static void reporterThread(PageAllocatorImpl& impl,
                            std::vector<ThreadStats>& stats,
                            size_t totalPages,
                            size_t reportIntervalMs) {
    using Clock = std::chrono::steady_clock;

    auto startTime = Clock::now();
    const double intervalSec = reportIntervalMs / 1000.0;

    // Previous-snapshot accumulators for delta computation
    const size_t n = stats.size();
    std::vector<uint64_t> prevAlloc(n, 0), prevFree(n, 0),
                          prevPages(n, 0), prevOom(n, 0);

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(reportIntervalMs));
        if (g_stop.load(std::memory_order_relaxed)) break;

        double elapsed = std::chrono::duration<double>(Clock::now() - startTime).count();

        uint64_t dAlloc = 0, dFree = 0, dPages = 0, dOom = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t a = stats[i].allocCalls    .load(std::memory_order_relaxed);
            uint64_t f = stats[i].freeCalls     .load(std::memory_order_relaxed);
            uint64_t p = stats[i].pagesAllocated.load(std::memory_order_relaxed);
            uint64_t o = stats[i].oomEvents     .load(std::memory_order_relaxed);
            dAlloc += a - prevAlloc[i];  prevAlloc[i] = a;
            dFree  += f - prevFree[i];   prevFree[i]  = f;
            dPages += p - prevPages[i];  prevPages[i] = p;
            dOom   += o - prevOom[i];    prevOom[i]   = o;
        }

        uint64_t freeCount = impl.countFreePages();
        double   oomPct    = (dAlloc > 0) ? 100.0 * dOom / dAlloc : 0.0;

        printf("[%8.3fs]  alloc: %s/s  free: %s/s  pages: %s/s  OOM: %5.2f%%  "
               "free pages: %s / %s\n",
               elapsed,
               fmtRate(dAlloc / intervalSec).c_str(),
               fmtRate(dFree  / intervalSec).c_str(),
               fmtRate(dPages / intervalSec).c_str(),
               oomPct,
               fmtNum(freeCount).c_str(),
               fmtNum(totalPages).c_str());
        fflush(stdout);
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    Config cfg = parseArgs(argc, argv);

    // Enforce limits
    if (cfg.numDomains == 0)        cfg.numDomains        = 1;
    if (cfg.bigPagesPerDomain == 0) cfg.bigPagesPerDomain = 1;
    if (cfg.threadsPerDomain == 0)  cfg.threadsPerDomain  = 1;
    if (cfg.maxBatch == 0)          cfg.maxBatch          = 1;
    if (cfg.maxBatch > HARD_MAX_BATCH) cfg.maxBatch       = HARD_MAX_BATCH;

    const size_t totalThreads = cfg.numDomains * cfg.threadsPerDomain;
    const size_t totalPages   = cfg.numDomains * cfg.bigPagesPerDomain
                                * PA::smallPagesPerBigPage;

    arch::stressSetProcessorCount(totalThreads);

    signal(SIGINT,  handleSigint);
    signal(SIGTERM, handleSigint);

    printf("=== CroCOS Page Allocator Stress Test ===\n");
    printf("  Domains:          %zu\n", cfg.numDomains);
    printf("  Big pages/domain: %zu  (%zu small pages each)\n",
           cfg.bigPagesPerDomain, PA::smallPagesPerBigPage);
    printf("  Total pages:      %s\n",  fmtNum(totalPages).c_str());
    printf("  Worker threads:   %zu  (%zu per domain)\n",
           totalThreads, cfg.threadsPerDomain);
    printf("  Max alloc batch:  %zu pages\n", cfg.maxBatch);
    printf("  Report interval:  %zu ms\n", cfg.reportIntervalMs);
    printf("  Press Ctrl+C to stop.\n\n");

    printf("Building allocator...\n");
    StressAllocatorImpl allocator(cfg);
    printf("Allocator ready. Starting workers.\n\n");

    printf("%-10s  %-15s  %-15s  %-17s  %-8s  %s\n",
           "[elapsed]", "alloc ops/s", "free ops/s", "pages alloc'd/s",
           "OOM %", "free pages");
    printf("%s\n", std::string(85, '-').c_str());
    fflush(stdout);

    // Allocate stats array (one entry per worker thread)
    std::vector<ThreadStats> stats(totalThreads);

    // Launch reporter first so it's ready before workers start flooding
    std::thread reporter([&]() {
        reporterThread(allocator.impl, stats, totalPages, cfg.reportIntervalMs);
    });

    // Launch worker threads
    std::vector<std::thread> workers;
    workers.reserve(totalThreads);
    for (size_t i = 0; i < totalThreads; i++) {
        workers.emplace_back([&, i]() {
            workerThread(allocator.impl, stats[i], cfg.maxBatch);
        });
    }

    // Wait for all workers to finish (they exit when g_stop is set)
    for (auto& w : workers) w.join();
    reporter.join();

    // Final summary
    uint64_t totalAlloc = 0, totalFree = 0, totalPg = 0, totalOom = 0;
    for (auto& s : stats) {
        totalAlloc += s.allocCalls    .load(std::memory_order_relaxed);
        totalFree  += s.freeCalls     .load(std::memory_order_relaxed);
        totalPg    += s.pagesAllocated.load(std::memory_order_relaxed);
        totalOom   += s.oomEvents     .load(std::memory_order_relaxed);
    }

    printf("\n=== Final Totals ===\n");
    printf("  Alloc calls:     %s\n", fmtNum(totalAlloc).c_str());
    printf("  Free calls:      %s\n", fmtNum(totalFree).c_str());
    printf("  Pages allocated: %s\n", fmtNum(totalPg).c_str());
    printf("  OOM events:      %s  (%.2f%%)\n",
           fmtNum(totalOom).c_str(),
           totalAlloc > 0 ? 100.0 * totalOom / totalAlloc : 0.0);
    printf("  Free pages now:  %s / %s\n",
           fmtNum(allocator.impl.countFreePages()).c_str(),
           fmtNum(totalPages).c_str());

    return 0;
}
