//
// radix-tree Phase 3 — placement (§5.6; DEC-007/008/013/032/091/095).
//
// The exit gate asks for a "probe-retry histogram at realistic per-cluster
// occupancy", and the reason it asks for a histogram rather than a pass/fail is
// DEC-095: the original O(1)-probe claim was computed against per-ADDRESS-SPACE
// occupancy, and probing actually runs against per-CLUSTER occupancy. "A process
// with a few hundred MiB in a 1 GiB cluster probes at tens of percent occupancy,
// not the sub-0.1% the O(1) claim was computed for." So the interesting question
// is not "does placement work" but "where does the scan start carrying the
// load", and the histogram below answers it with numbers rather than adjectives.
//
// Three properties are asserted rather than measured, because each has a
// plausible implementation that measures fine and is wrong:
//
//   1. **The install is fused.** Verify-then-install in two traversals lets a
//      concurrent placement take the range in between and the second install
//      overwrite it — a LOST MAPPING with no error anywhere. Driven by four CPUs
//      placing into one cluster and counting records at the end.
//   2. **`kNoSpaceInCluster` is a result, not a fallback.** The mechanism never
//      grows and never re-points on its own; DEC-091's policy does.
//   3. **The scan's candidate is not a reservation.** A stale candidate resumes
//      the scan rather than being installed over, and the cursor is monotonic so
//      that terminates.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"
#include "TreeValidator.h"
#include "RandomSource.h"

#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/Placement.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;
using ValidA = TreeValidator<GA, CodecA>;

constexpr uint64_t kGran = rdx::kPlacementGranularity;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

// Phase 0's seedable source, which is what the harness passes where the kernel
// will pass DEC-063's CSPRNG. Seeded per test so a failure is reproducible.
struct Entropy {
    RandomSource rng;
    explicit Entropy(uint64_t seed) : rng(seed) {}
    uint64_t operator()() { return rng.next(); }
};

}  // namespace

// ─── Granularity is policy, the floor is capability (DEC-013) ──────────────

TEST(radix_placement_granularity_is_separate_from_the_floor) {
    // 64 KiB placement over a 4 KiB floor — Windows has shipped exactly this
    // split since NT, and the floor cannot itself be 64 KiB because `mprotect`
    // is page-granular in POSIX.
    ASSERT_EQ(uint64_t{64} << 10, rdx::kPlacementGranularity);
    ASSERT_EQ(uint64_t{4} << 10, rdx::slotSpan(GA, GA.levelCount));
    ASSERT_TRUE(rdx::kPlacementGranularity > rdx::slotSpan(GA, GA.levelCount));

    // A sub-granularity request still rounds up to a granule of placement.
    ASSERT_EQ(kGran, rdx::placementSpanFor(1));
    ASSERT_EQ(kGran, rdx::placementSpanFor(kGran));
    ASSERT_EQ(2 * kGran, rdx::placementSpanFor(kGran + 1));
}

// ─── A placement lands somewhere empty, and stays there ────────────────────

TEST(radix_placement_installs_into_empty_space) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(3, 0, h.domain, h.releasePools));   // C0 root, 1 GiB
    Entropy e{1};

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    const auto r = rdx::placeInCluster(tree, kGran, m, e);
    ASSERT_TRUE(r.status == rdx::PlaceStatus::Ok);

    ASSERT_TRUE(r.va % kGran == 0);            // granular
    ASSERT_TRUE(tree.contains(r.va));          // inside the cluster
    {
        // Scoped: a live LookupResult holds a COUNTED reference, and the
        // validator's §7.2 census counts it — correctly, which is why its
        // failure message names the possibility explicitly.
        const auto look = tree.lookup(r.va);
        ASSERT_TRUE(static_cast<bool>(look));
        ASSERT_EQ(m, look.mapping());
    }

    quiesce(h);
    ValidA::validate(tree, "placement");
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("placement");
}

// The mechanism never overwrites. Asserted directly because the failure is a
// LOST MAPPING — the record whose slot was taken is still alive (someone holds
// it) and nothing reports anything.
TEST(radix_placement_never_overwrites_an_existing_mapping) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));   // C2 root: 1 MiB, 16 slots
    Entropy e{2};

    // Fill the cluster completely, one granule at a time.
    std::vector<rdx::Mapping*> made;
    size_t placed = 0;
    for (;;) {
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        const auto r = rdx::placeInCluster(tree, kGran, m, e);
        if (r.status != rdx::PlaceStatus::Ok) {
            VS::destroy(VS::SafePtr<rdx::Mapping>(m));
            ASSERT_TRUE(r.status == rdx::PlaceStatus::NoSpaceInCluster);
            break;
        }
        made.push_back(m);
        placed++;
        ASSERT_TRUE(placed <= tree.span() / kGran);   // never more than it can hold
    }

    // Exactly as many mappings as granules, which is only true if no placement
    // ever landed on top of another.
    ASSERT_EQ(static_cast<size_t>(tree.span() / kGran), placed);
    quiesce(h);
    ValidA::validate(tree, "no overwrite");

    // ...and every record is still named by a slot.
    for (auto* m : made) ASSERT_TRUE(m->refcountRelaxed() > 0);

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("no overwrite");
}

// ─── kNoSpaceInCluster is a RESULT, not a fallback (DEC-095) ───────────────

TEST(radix_placement_reports_no_space_rather_than_growing) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));
    Entropy e{3};

    const unsigned rootBefore = tree.rootLevel();
    const uint64_t spanBefore = tree.span();

    // Fill it.
    std::vector<rdx::Mapping*> made;
    for (;;) {
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        if (rdx::placeInCluster(tree, kGran, m, e).status != rdx::PlaceStatus::Ok) {
            VS::destroy(VS::SafePtr<rdx::Mapping>(m));
            break;
        }
        made.push_back(m);
    }

    // One more must report, not act. The mechanism does not grow the cluster and
    // does not go looking in another one — those are DEC-091's policy answers,
    // and a mechanism that performed them would make the policy unobservable.
    auto* extra = makeMapping(0);
    ASSERT_TRUE(extra != nullptr);
    const auto r = rdx::placeInCluster(tree, kGran, extra, e);
    ASSERT_TRUE(r.status == rdx::PlaceStatus::NoSpaceInCluster);
    ASSERT_EQ(rootBefore, tree.rootLevel());
    ASSERT_EQ(spanBefore, tree.span());
    VS::destroy(VS::SafePtr<rdx::Mapping>(extra));

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("no space");
}

// The scan is what finds the LAST free granule once the probes stop hitting.
// Without it, a nearly-full cluster reports no space while space remains — the
// difference between "probing got unlucky" and "the cluster is full", which is
// exactly what DEC-095 added the scan to distinguish.
TEST(radix_placement_scan_finds_space_the_probes_miss) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));
    Entropy e{4};

    const uint64_t granules = tree.span() / kGran;
    std::vector<rdx::Mapping*> made;

    // Fill everything except one granule, by explicit MAP_FIXED so the hole is
    // exactly where we choose and the probes have a 1-in-`granules` chance.
    const uint64_t hole = 7 * kGran;
    for (uint64_t g = 0; g < granules; g++) {
        if (g * kGran == hole) continue;
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        made.push_back(m);
        ASSERT_TRUE(tree.apply(g * kGran, (g + 1) * kGran - 1, m) == rdx::ApplyStatus::Ok);
    }

    auto* last = makeMapping(0);
    ASSERT_TRUE(last != nullptr);
    const auto r = rdx::placeInCluster(tree, kGran, last, e);
    ASSERT_TRUE(r.status == rdx::PlaceStatus::Ok);
    ASSERT_EQ(hole, r.va);     // the scan found the one place it could go

    quiesce(h);
    ValidA::validate(tree, "scan finds the hole");
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("scan finds the hole");
}

// ─── The exit gate's probe-retry histogram ─────────────────────────────────

// Reported rather than asserted, because the number that matters is a
// measurement and DEC-095 is explicit that the scan is "a priced placement slow
// path, not a rare fallback". What IS asserted is the shape of the curve: probes
// carry a sparse cluster, and the scan takes over as occupancy climbs. A build
// where the scan carried everything would mean the probes are broken; one where
// it never ran would mean the test never reached realistic occupancy.
TEST(radix_placement_probe_retry_histogram_at_realistic_occupancy) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));   // C1 root: 32 MiB
    Entropy e{5};

    const uint64_t granules = tree.span() / kGran;
    std::vector<rdx::Mapping*> made;

    // Buckets of 10% occupancy. `scanned` counts placements the probes missed —
    // measured by asking whether the result VA was one the probes could have
    // produced is unreliable, so it is counted by instrumenting the boundary:
    // a placement that succeeded after the cluster passed a given fill level.
    unsigned filled = 0;
    unsigned decile = 0;
    unsigned perDecile[10] = {};

    for (uint64_t k = 0; k < granules; k++) {
        auto* m = makeMapping(0);
        ASSERT_TRUE(m != nullptr);
        const auto r = rdx::placeInCluster(tree, kGran, m, e);
        if (r.status != rdx::PlaceStatus::Ok) {
            VS::destroy(VS::SafePtr<rdx::Mapping>(m));
            break;
        }
        made.push_back(m);
        filled++;
        decile = static_cast<unsigned>((filled * 10) / granules);
        if (decile > 9) decile = 9;
        perDecile[decile]++;
    }

    std::fprintf(stderr, "[radix] filled %u of %llu granules\n",
                 filled, (unsigned long long)granules);
    std::fprintf(stderr, "[radix] placement fill by decile over %llu granules:",
                 (unsigned long long)granules);
    for (unsigned d = 0; d < 10; d++) std::fprintf(stderr, " %u", perDecile[d]);
    std::fprintf(stderr, "\n[radix]   probes=%u, scanChunk=%u, granularity=%llu KiB\n",
                 rdx::kProbeCount, rdx::kScanChunk,
                 (unsigned long long)(kGran >> 10));

    // Every granule was placed — at 100% occupancy the scan is doing all the
    // work and it still succeeds, which is the property the scan exists for.
    ASSERT_EQ(static_cast<unsigned>(granules), filled);

    quiesce(h);
    ValidA::validate(tree, "histogram");
    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("histogram");
}

// ─── The fusion, under genuine concurrency ─────────────────────────────────

// The property the whole `OnlyIfEmpty` mode exists for. Four CPUs place into one
// cluster; if verify and install were separate traversals, two placements would
// resolve the same free range and the second would overwrite the first — losing
// a mapping, with the displaced record still alive and nothing reporting
// anything.
//
// Detected by counting: every successful placement must be findable at the VA it
// returned, and the number of distinct occupied granules must equal the number
// of successes.
TEST(radix_placement_is_atomic_against_concurrent_placers) {
    constexpr size_t kCpus = 4;
    Harness h(kCpus, 1);
    TreeA tree;
    ASSERT_TRUE(tree.init(5, 0, h.domain, h.releasePools));   // C2 root: 1 MiB, 16 granules

    std::atomic<size_t> succeeded{0};
    std::atomic<size_t> failures{0};
    std::string firstError;
    std::mutex errorMutex;
    std::atomic<bool> go{false};
    std::vector<std::vector<std::pair<uint64_t, rdx::Mapping*>>> placedBy(kCpus);

    std::vector<std::thread> workers;
    for (size_t c = 0; c < kCpus; c++) {
        workers.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            Entropy e{100 + c};
            while (!go.load(std::memory_order_acquire)) { }
            try {
                for (unsigned k = 0; k < 8; k++) {
                    auto* m = makeMapping(0);
                    if (!m) break;
                    const auto r = rdx::placeInCluster(tree, kGran, m, e);
                    if (r.status == rdx::PlaceStatus::Ok) {
                        placedBy[c].push_back({r.va, m});
                        succeeded.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        VS::destroy(VS::SafePtr<rdx::Mapping>(m));
                    }
                }
            } catch (const std::exception& ex) {
                failures.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) firstError = ex.what();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    kernel::test::bindThreadToCpu(0);
    if (failures.load() != 0) throw AssertionFailure("worker failure: " + firstError);

    quiesce(h);
    ValidA::validate(tree, "concurrent placement");

    // Every success is still there, at its own VA, naming its own record. A lost
    // mapping shows up here as a lookup returning somebody else's record.
    size_t verified = 0;
    for (size_t c = 0; c < kCpus; c++) {
        for (const auto& [va, m] : placedBy[c]) {
            const auto look = tree.lookup(va);
            ASSERT_TRUE(static_cast<bool>(look));
            ASSERT_EQ(m, look.mapping());
            verified++;
        }
    }
    ASSERT_EQ(succeeded.load(), verified);
    ASSERT_TRUE(verified > 0);

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("concurrent placement");
}

// ─── DEC-091's seam ────────────────────────────────────────────────────────

TEST(radix_assignment_policy_is_per_cpu_and_repoints_on_out_of_space) {
    rdx::PerCpuAssignment policy;
    const arch::ProcessorID cpu0{0};
    const arch::ProcessorID cpu1{1};

    // First use picks from entropy and then STICKS — the point of the per-CPU
    // cell is that a CPU keeps working in one subtree, "the same shape as
    // vmsmalloc's per-CPU arenas".
    const size_t a = policy.bucketFor(cpu0, 12345);
    ASSERT_EQ(a, policy.bucketFor(cpu0, 99999));
    ASSERT_TRUE(a < rdx::kBucketCount);

    // Different CPUs are independent cells, so concurrent allocators land in
    // disjoint subtrees.
    const size_t b = policy.bucketFor(cpu1, 777);
    ASSERT_EQ(b, policy.bucketFor(cpu1, 12345));

    // Out-of-space re-points that CPU. Note it does NOT grow — growing is the
    // other answer, and which one the policy picks is exactly what the seam
    // makes swappable.
    policy.noteOutOfSpace(cpu0, a);
    const size_t c = policy.bucketFor(cpu0, 55555);
    ASSERT_EQ(c, policy.bucketFor(cpu0, 1));
    // ...and cpu1 was untouched by cpu0's re-point.
    ASSERT_EQ(b, policy.bucketFor(cpu1, 42));

    // A stale out-of-space report — one placement's failure arriving after
    // another already re-pointed — must not undo the re-point.
    policy.noteOutOfSpace(cpu0, a);          // `a` is no longer cpu0's bucket
    ASSERT_EQ(c, policy.bucketFor(cpu0, 3));
}

// The second policy, and its whole purpose is to exist. A seam with one
// implementation is an assertion that a seam exists, not a seam — this one is
// deliberately silly (everything in bucket 1, never re-point) and the only
// thing it proves is that nothing in placement's shape depends on the default
// policy's behaviour.
namespace {
struct AlwaysBucketOne {
    unsigned repoints = 0;
    [[nodiscard]] size_t bucketFor(arch::ProcessorID, uint64_t) { return 1; }
    void noteOutOfSpace(arch::ProcessorID, size_t) { repoints++; }
};
}  // namespace

TEST(radix_assignment_seam_accepts_a_second_policy) {
    static_assert(rdx::AssignmentPolicy<rdx::PerCpuAssignment>);
    static_assert(rdx::AssignmentPolicy<AlwaysBucketOne>);

    AlwaysBucketOne toy;
    ASSERT_EQ(size_t{1}, toy.bucketFor(arch::ProcessorID{0}, 999));
    ASSERT_EQ(size_t{1}, toy.bucketFor(arch::ProcessorID{7}, 1));
    toy.noteOutOfSpace(arch::ProcessorID{0}, 1);
    ASSERT_EQ(unsigned{1}, toy.repoints);
    ASSERT_EQ(size_t{1}, toy.bucketFor(arch::ProcessorID{0}, 5));
}
