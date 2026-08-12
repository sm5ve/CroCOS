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
#include <unistd.h>
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
//
// Templated on the arm (Item B): `false` is the classic counted lookup, whose
// section lives inside `resumeDescent`; `true` is the borrow shape the fast
// fault path would use — one `BorrowWindow` (section + DEC-060 pump) around
// each borrow, so both arms pay one section entry and one pump-gate load per
// operation and the measured difference is the Mapping pin pair plus the
// window's own scaffolding. A template rather than a parameter so neither arm
// carries a branch in its hot loop.
template <bool Borrow = false>
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
                if constexpr (Borrow) {
                    rdx::BorrowWindow window(block.domain);
                    auto r = cache.borrow(block, va);
                    if (r) hits++;
                } else {
                    auto r = cache.lookup(block, va);
                    if (r) hits++;
                }
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

// The report body, shared by the two arms. A function pointer rather than a
// template so the ~60 lines of population and printing exist once; the
// per-lookup code is inside `runTrial<Borrow>`, so the indirection is paid per
// TRIAL, not per operation.
void scalingReport(const char* arm,
                   double (*trial)(CacheA&, BlockA&, size_t, uint64_t)) {
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

    std::printf("\n  radix %s throughput vs thread count"
                " (native, real caches — the one thing -icount cannot see)\n", arm);
    // This file is compiled into both the ASan gate runner and the -O2
    // uninstrumented bench runner, and a saved log does not say which produced
    // it — so the binary states its own shape rather than assuming one.
#if !defined(CROCOS_BENCH_ASAN)
#  if defined(__has_feature)
#    if __has_feature(address_sanitizer)
#      define CROCOS_BENCH_ASAN 1
#    endif
#  endif
#  if !defined(CROCOS_BENCH_ASAN) && defined(__SANITIZE_ADDRESS__)
#    define CROCOS_BENCH_ASAN 1
#  endif
#  if !defined(CROCOS_BENCH_ASAN)
#    define CROCOS_BENCH_ASAN 0
#  endif
#endif
#if CROCOS_BENCH_ASAN
    std::printf("  NOTE: this runner is instrumented (AddressSanitizer), so the\n"
                "  absolute ns are dominated by instrumentation. Only the SCALING\n"
                "  column is being claimed, and only against another run of this\n"
                "  same binary shape on this same machine.\n");
#else
    std::printf("  NOTE: uninstrumented -O2 build, release-shaped checks. Absolute\n"
                "  ns are comparable only against runs of this same binary shape on\n"
                "  this same machine; they are not portable and are not claimed to be.\n");
#endif
    std::printf("      threads |  min ns/lookup (aggregate) |  min ns/lookup/thread\n");

    constexpr unsigned kTrials = 5;
    for (size_t threads : kThreadCounts) {
        // One warm-up so descent-cache entries are installed and the measured
        // trials are steady state rather than cold installs.
        (void)trial(*cache, *space.block, threads, regionSpan);

        double best = 0.0;
        for (unsigned t = 0; t < kTrials; t++) {
            const double ns = trial(*cache, *space.block, threads, regionSpan);
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

}  // namespace

TEST(radix_lookup_contention_scaling_report) {
    scalingReport("lookup", &runTrial<false>);
}

// Item B's A/B partner: the same workload through `borrow` under a per-lookup
// `BorrowWindow`. Run BOTH reports from the same binary, alternating, and read
// the difference at a fixed thread count — that difference is what the counted
// reference actually costs, measured rather than argued.
TEST(radix_borrow_contention_scaling_report) {
    scalingReport("borrow", &runTrial<true>);
}

// ─── Profiling loop: the hit path, held on-CPU for `sample` ────────────────
//
// The throughput report above finishes its single-thread phase in ~a tenth of a
// second, which is an order of magnitude under `sample`'s default window — a
// profiler attached to it mostly captures thread startup and the harness. This
// keeps the SAME steady-state hit path (same geometry, same 8 registered CPUs so
// `tryAdvance` walks the same reader slots, same stride, same region span) on
// one thread for a requested number of seconds, so that
//
//   CROCOS_RADIX_PROFILE_SECS=20 ./KernelRadixBenchRunner radix_lookup_profile_loop &
//   sample <pid> 10 -file /tmp/radix.txt
//   python3 tools/sample_to_flamegraph.py /tmp/radix.txt
//
// yields thousands of on-CPU samples that are all lookup. It prints its own
// ns/lookup at the end as a cross-check against the report's single-thread row;
// if the two disagree badly, the profile captured something the bench does not
// run, and the flame graph should be distrusted accordingly.
//
// Opt-in via its own variable rather than CROCOS_RADIX_BENCH because a run of
// the full bench should not also burn N seconds feeding a profiler nobody
// attached.

TEST(radix_lookup_profile_loop) {
    const char* secsEnv = std::getenv("CROCOS_RADIX_PROFILE_SECS");
    if (secsEnv == nullptr) {
        std::printf("\n  SKIP profile loop — set CROCOS_RADIX_PROFILE_SECS=<seconds> to run it\n\n");
        return;
    }
    const long secs = std::strtol(secsEnv, nullptr, 10);
    if (secs <= 0) throw AssertionFailure("profile loop: CROCOS_RADIX_PROFILE_SECS must be a positive integer");

    Harness h(kMaxThreads, 1);
    Space   space(kMaxThreads);
    auto    cache = std::make_unique<CacheA>();

    // One thread's region, populated exactly as the report populates it, so
    // every lookup is a hit and the measured path is the report's hit path.
    const uint64_t regionSpan = uint64_t{1} << 22;
    for (uint64_t off = 0; off < regionSpan; off += kPage * 3) {
        rdx::Mapping* m = makeMapping(off);
        if (m == nullptr) throw AssertionFailure("profile loop: out of mappings");
        space.map(off, off + kPage - 1, m);
    }

    kernel::test::bindThreadToCpu(0);

    std::printf("\n  profile loop: pid %d, ~%ld s of single-thread hit-path lookups — attach `sample` now\n",
                static_cast<int>(getpid()), secs);
    std::fflush(stdout);

    // Deadline checked once per batch so the clock read stays out of the
    // profiled loop (at ~100 ns/lookup a batch is ~0.4 ms).
    constexpr unsigned kBatch = 4096;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    uint64_t total = 0, hits = 0, va = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        for (unsigned k = 0; k < kBatch; k++) {
            auto r = cache->lookup(*space.block, va);
            if (r) hits++;
            va += kPage * 3;
            if (va >= regionSpan) va = 0;
        }
        total += kBatch;
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Every probed address was mapped above, so a miss means the loop wandered
    // off the population pattern and profiled a different path than claimed.
    if (hits != total) throw AssertionFailure("profile loop: a lookup missed — wrong path profiled");

    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    std::printf("  profile loop: %llu lookups, %.1f ns/lookup (cross-check vs the report's 1-thread row)\n\n",
                static_cast<unsigned long long>(total),
                ns / static_cast<double>(total));
}

// ─── Calibration: what does a contended RMW actually cost here? ────────────
//
// The lookup benchmark above cannot attribute its degradation on its own. Every
// lookup performs BOTH the three `DescentCacheStats` bumps and DEC-060's
// `tryAdvance`, and `tryAdvance` walks every CPU's reader slot — which is also
// shared-line traffic. So "throughput falls as threads rise" is consistent with
// the counters, with the pump, or with both, and picking one without evidence is
// how a performance story becomes folklore.
//
// This supplies the missing constant: the cost of one `fetch_add` on a shared
// line versus one on a private line, at the same thread counts, on this machine.
// With it, the counters' predicted contribution is 3 x (shared - private) per
// lookup, which can be checked against what the A/B of an actual fix recovers.
// If the fix recovers far more than predicted, the pump was the bigger half.
//
// Deliberately NOT a claim about the radix tree. It is a machine constant.

TEST(radix_contended_atomic_calibration_report) {
    if (const char* on = std::getenv("CROCOS_RADIX_BENCH"); on == nullptr || on[0] != '1') {
        return;   // the lookup benchmark above already explains the opt-in
    }

    constexpr unsigned kIters = 2000000;
    constexpr unsigned kTrials = 5;

    // One shared counter, versus one cache-line-padded counter per thread. The
    // padding is the whole point of the comparison: same instruction, same count,
    // differing only in whether the line is contended.
    struct alignas(128) Padded { std::atomic<uint64_t> v{0}; };

    std::printf("\n  contended vs private atomic fetch_add"
                " (machine constant, not a radix figure)\n");
    // PER-THREAD nanoseconds, deliberately, so these compose with the lookup
    // table above — whose per-thread column is what a single faulting CPU
    // actually experiences. Aggregate figures would not subtract correctly.
    std::printf("      threads | shared ns/op/thr | private ns/op/thr |  tax per RMW\n");

    for (size_t threads : kThreadCounts) {
        double bestShared = 0.0, bestPrivate = 0.0;

        for (unsigned trial = 0; trial < kTrials; trial++) {
            for (int mode = 0; mode < 2; mode++) {          // 0 = shared, 1 = private
                std::atomic<uint64_t>    shared{0};
                std::vector<Padded>      privates(threads);
                std::vector<std::thread> workers;
                std::atomic<bool>        go{false};
                std::atomic<size_t>      ready{0};

                for (size_t t = 0; t < threads; t++) {
                    workers.emplace_back([&, t] {
                        ready.fetch_add(1, std::memory_order_release);
                        while (!go.load(std::memory_order_acquire)) { }
                        auto& slot = (mode == 0) ? shared : privates[t].v;
                        for (unsigned k = 0; k < kIters; k++) {
                            slot.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                }
                while (ready.load(std::memory_order_acquire) != threads) { }
                const auto t0 = std::chrono::steady_clock::now();
                go.store(true, std::memory_order_release);
                for (auto& w : workers) w.join();
                const auto t1 = std::chrono::steady_clock::now();

                const double ns =
                    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t1 - t0).count()) / static_cast<double>(threads * kIters);
                double& best = (mode == 0) ? bestShared : bestPrivate;
                if (best == 0.0 || ns < best) best = ns;
            }
        }

        const double sharedPerThread  = bestShared  * static_cast<double>(threads);
        const double privatePerThread = bestPrivate * static_cast<double>(threads);
        std::printf("      %7zu | %16.2f | %17.2f | %9.1f ns\n",
                    threads, sharedPerThread, privatePerThread,
                    sharedPerThread - privatePerThread);
    }

    std::printf("\n  A hit-path lookup performs THREE shared bumps (lookups,\n"
                "  freshnessLoads, hits), so DescentCacheStats' predicted per-lookup\n"
                "  cost is 3 x the tax column, in the same per-thread units as the\n"
                "  lookup table above. Subtract that from the lookup table's per-thread\n"
                "  degradation to see how much of the problem the counters are NOT.\n"
                "  Whatever remains is elsewhere — DEC-060's tryAdvance walks every\n"
                "  CPU's reader slot on every lookup, and those slots are actively\n"
                "  written by the other threads.\n\n");
}
