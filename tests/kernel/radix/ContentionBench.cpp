//
// The instrument this project did not have: SMP contention, on real silicon.
//
// ─── Why it exists ────────────────────────────────────────────────────────
//
// D-075 priced a page-fault lookup at ~799 instructions under `-icount` and then
// had to report that the most likely real cost was one it could not measure.
// `DescentCacheStats` is a single SHARED struct of sixteen atomics whose
// `fetch_add` sites are unconditional in every build, and a hit-path lookup bumps
// three of them. `kCacheAccounting` is RELAXED, which drops the fences but not
// the lock prefix, so every CPU performs three locked read-modify-writes on the
// SAME cache lines on every page fault.
//
// That costs ~10 instructions and hundreds of cycles, and the gap between those
// two numbers is the entire problem. Every measuring tool the project had was on
// the wrong side of it:
//
//   - `-icount` counts instructions and forces single-threaded TCG. A locked RMW
//     costs the same there whether zero or sixty-four cores contend for the line.
//   - TCG models no cache at all, which is why ITEM-055's false-sharing question
//     was consciously retained as unanswerable rather than settled.
//
// So a fix for the counters could be argued but not falsified, which is the
// condition under which performance work turns into folklore.
//
// **The userspace harness runs natively.** It is the one place in this project
// where radix code executes on a real CPU with a real cache and real coherence
// traffic. That makes contention measurable here and nowhere else, so this file
// measures it here.
//
// ─── What it measures, and the claim it deliberately does NOT make ────────
//
// Lookup throughput at several thread counts, over disjoint per-thread regions.
//
// **It does not report a scaling efficiency, because it cannot honestly.** The
// first version did, and the number was partly an artifact. `bindThreadToCpu` in
// the mock environment binds a thread to a LOGICAL cpu — it selects which mock
// `CpuLocal` page and RCU reader slot the thread uses — and does no physical
// affinity at all. macOS grants no usable affinity on Apple Silicon anyway. So on
// a 4+4 performance/efficiency machine, thread placement is the scheduler's
// choice, and a "scaling curve" conflates three things: coherence traffic, core
// asymmetry, and placement luck. The -O2 run that exposed this was
// non-monotonic — two and four threads slower in AGGREGATE than one — which is
// not a contention shape, it is a P-core/E-core shape.
//
// So this prints per-thread-count timings and nothing derived. The sound way to
// use it is an **A/B at a FIXED thread count**: build the two variants, run them
// alternately on an otherwise idle machine, and compare minima. Scheduler noise
// and core asymmetry then fall on both arms rather than on the conclusion.
//
// Minima rather than means, for the reason the `-icount` probes already use them
// (P4-ITEM-002): a descheduled thread inflates a sample by the whole quantum, and
// that contamination dilutes with sample count instead of averaging out. The
// minimum over repeated trials is the least contaminated path.
//
// Reading absolute cost off this would be wrong twice more: the mock substrate's
// `ensureTLBEntryFresh` is a no-op, and an M1's core-to-core latency is not a
// server's.
//
// ─── Why it is a report and not a gate ────────────────────────────────────
//
// Wall-clock assertions on a shared machine do not survive contact with load.
// This project already learned that once: sixteen Core TSan stress tests were
// diagnosed as machine-load starvation of wall-clock timeouts rather than
// flakiness, and are gated behind `CROCOS_SKIP_TSAN_STRESS` for it. A benchmark
// that fails when someone opens a browser would be deleted within a week, and
// deserves to be.
//
// So this asserts only what is load-INDEPENDENT — that every thread completed the
// work it was given — and prints the timings for a human. It is additionally
// opt-in via `CROCOS_RADIX_BENCH=1`, because burning seconds of the default gate
// to produce numbers nobody is reading is its own kind of waste.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/AddressSpace.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/DescentCache.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;
using StoreA = rdx::ControlBlockStore<GA, CodecA>;
using BlockA = rdx::ControlBlock<GA, CodecA>;
using CacheA = rdx::DescentCache<GA, CodecA>;

constexpr uint64_t kPage = 4096;
constexpr size_t   kTestRecords = 64;

// Thread counts to sweep. Powers of two up to the M1's performance-core count;
// the interesting comparison is 1 against the largest, and the intermediate
// points are what distinguish a smooth roll-off from a cliff.
constexpr size_t kThreadCounts[] = {1, 2, 4, 8};
constexpr size_t kMaxThreads = 8;

// Lookups per thread per trial. Large enough that thread start-up and the clock
// read are noise; small enough that the whole sweep stays a few seconds.
constexpr unsigned kLookupsPerThread = 200000;

struct Space {
    StoreA  store;
    BlockA* block = nullptr;

    explicit Space(size_t cpus, uint64_t span = uint64_t{1} << 30) {
        if (rdx::createAddressSpace(store, kernel::numa::DomainID{0}, cpus,
                                    kTestRecords, block) != rdx::CreateStatus::Ok) {
            throw AssertionFailure("contention bench: address-space creation failed");
        }
        if (block->clusters.ensureCovers(0, span - 1) != rdx::ClusterStatus::Ok) {
            throw AssertionFailure("contention bench: cluster creation failed");
        }
    }
    ~Space() { if (block) rdx::destroyAddressSpace(store, block); }

    void map(uint64_t lo, uint64_t hi, rdx::Mapping* m) {
        TreeA t = block->clusters.treeFor(rdx::bucketIndexFor<GA>(lo));
        if (t.apply(lo, hi, m) != rdx::ApplyStatus::Ok) {
            throw AssertionFailure("contention bench: apply failed");
        }
    }
};

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// One trial: `threads` bound workers, each doing `kLookupsPerThread` lookups over
// its OWN disjoint VA region. Disjoint on purpose — it removes tree-level claim
// contention from the measurement so that what remains is the per-fault
// bookkeeping every lookup performs regardless of where it lands. If throughput
// still degrades with thread count under disjoint keys, the degradation is not
// the data structure.
//
// Returns nanoseconds per lookup, aggregated across threads.
double runTrial(CacheA& cache, BlockA& block, size_t threads, uint64_t regionSpan) {
    std::vector<std::thread> workers;
    std::atomic<bool>   go{false};
    std::atomic<size_t> ready{0};
    std::atomic<size_t> completed{0};
    std::atomic<size_t> found{0};

    for (size_t t = 0; t < threads; t++) {
        workers.emplace_back([&, t] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(t));
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { }

            const uint64_t base = regionSpan * t;
            unsigned hits = 0;
            // A stride that is not a multiple of the cache-entry index stride, so
            // the four-entry set is exercised rather than one entry answering
            // everything — otherwise this measures a single hot entry and not a
            // realistic descent.
            uint64_t va = base;
            for (unsigned k = 0; k < kLookupsPerThread; k++) {
                auto r = cache.lookup(block, va);
                if (r) hits++;
                va += kPage * 3;
                if (va - base >= regionSpan) va = base;
            }
            found.fetch_add(hits, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_release);
        });
    }

    while (ready.load(std::memory_order_acquire) != threads) { }
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    const auto t1 = std::chrono::steady_clock::now();
    kernel::test::bindThreadToCpu(0);

    // The only assertion, and it is load-independent: every worker finished the
    // work it was given. A timing number from a trial where a thread died is not
    // a slow result, it is a wrong one.
    if (completed.load() != threads) {
        throw AssertionFailure("contention bench: a worker did not complete");
    }

    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(threads * kLookupsPerThread);
}

}  // namespace

TEST(radix_lookup_contention_scaling_report) {
    if (const char* on = std::getenv("CROCOS_RADIX_BENCH"); on == nullptr || on[0] != '1') {
        std::printf("\n  SKIP contention benchmark — set CROCOS_RADIX_BENCH=1 to run it\n"
                    "  (opt-in: it burns several seconds and its numbers are only\n"
                    "   meaningful compared against another run on the same machine)\n\n");
        return;
    }

    Harness h(kMaxThreads, 1);
    Space   space(kMaxThreads);
    auto    cache = std::make_unique<CacheA>();

    // One mapping per page across each thread's region, so every lookup finds
    // something and the measured path is the HIT path — the one every page fault
    // takes and the one whose per-fault bookkeeping is under suspicion.
    const uint64_t regionSpan = uint64_t{1} << 22;          // 4 MiB per thread
    for (size_t t = 0; t < kMaxThreads; t++) {
        const uint64_t base = regionSpan * t;
        for (uint64_t off = 0; off < regionSpan; off += kPage * 3) {
            rdx::Mapping* m = makeMapping(base + off);
            if (m == nullptr) throw AssertionFailure("contention bench: out of mappings");
            space.map(base + off, base + off + kPage - 1, m);
        }
    }

    std::printf("\n  radix lookup throughput vs thread count"
                " (native, real caches — the one thing -icount cannot see)\n");
    std::printf("  NOTE: this runner is a Debug build under AddressSanitizer, so the\n"
                "  absolute ns are dominated by instrumentation. Only the SCALING\n"
                "  column is being claimed, and only against another run of this\n"
                "  same binary shape on this same machine.\n");
    std::printf("      threads |  min ns/lookup (aggregate) |  min ns/lookup/thread\n");

    constexpr unsigned kTrials = 5;
    for (size_t threads : kThreadCounts) {
        // One warm-up so descent-cache entries are installed and the measured
        // trials are steady state rather than cold installs.
        (void)runTrial(*cache, *space.block, threads, regionSpan);

        double best = 0.0;
        for (unsigned t = 0; t < kTrials; t++) {
            const double ns = runTrial(*cache, *space.block, threads, regionSpan);
            if (best == 0.0 || ns < best) best = ns;
        }
        // Aggregate (wall time over TOTAL lookups) and its per-thread inverse.
        // Both raw. No efficiency column: see the header — without physical
        // affinity, a derived scaling number would be reporting the macOS
        // scheduler as though it were coherence traffic.
        std::printf("      %7zu | %26.1f | %21.1f\n",
                    threads, best, best * static_cast<double>(threads));
    }

    std::printf("\n  Per-thread regions are DISJOINT, so anything that degrades with\n"
                "  thread count is not the tree — it is the per-fault bookkeeping every\n"
                "  lookup performs, with DescentCacheStats' shared atomics the standing\n"
                "  suspect. But do not read a scaling claim off this table: threads are\n"
                "  not pinned to physical cores (they cannot be, here), so P-core vs\n"
                "  E-core placement is mixed into every row.\n"
                "  Sound use: A/B two builds at ONE thread count, alternating, idle\n"
                "  machine, compare minima.\n\n");
}
