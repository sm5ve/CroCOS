//
// radix-tree Phase 3 — ordered occupied-range enumeration (§5.5, DEC-083).
//
// "the primitive span `munmap`/`mprotect` iterate with, cluster-stitched by the
// policy layer per DEC-058."
//
// Three claims, in decreasing order of how obvious they are:
//
//   1. **Ascending VA order, every occupied range, nothing else.** The easy
//      half, and the one a naive implementation gets right.
//   2. **Chunked**: at most `scanChunk` slots per read section, resumed across
//      the close by a **VA cursor and nothing else**. §7.3 forbids uncounted
//      pointers crossing a close, not addresses — carrying a node pointer in the
//      cursor would be §3.1's rejected token shape in a second place. The
//      failure this prevents has no functional symptom: a whole-cluster
//      enumeration in one section is ~10^3 nodes of non-preemptible,
//      domain-wide EBR-stalling work, and it *works perfectly*.
//   3. **Sparse regions cost what they should.** An empty C0 slot must be
//      skipped in one step, not crawled at the 4 KiB floor — 8192 steps per
//      32 MiB hole is the difference between an enumeration and a hang, and it
//      is invisible to any correctness check.
//
// Claim 3 is why the scan reports the extent of the emptiness it proves rather
// than just "not mapped here", and it is asserted directly below by counting
// steps rather than inferred from a timing.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include "RadixHarness.h"

#include <mem/radix/CoreTree.h>

#include <cstdio>
#include <vector>

using namespace CroCOSTest;
using namespace CroCOSTest::radix;
namespace rdx = kernel::mm::radix;

namespace {

constexpr auto GA = rdx::kAmd64Geometry;
using CodecA = rdx::HarnessSlotCodec<GA>;
using TreeA  = rdx::CoreTree<GA, CodecA>;

constexpr uint64_t kPage = 4096;

rdx::Mapping* makeMapping(uint64_t baseVA) {
    auto p = VS::tryMake<rdx::Mapping>(nullptr, uint64_t{0}, baseVA,
                                       rdx::Protection::Read, rdx::Protection::Read);
    if (!p) return nullptr;
    return static_cast<rdx::Mapping*>(p.raw());
}

struct Range {
    rdx::Mapping* m;
    uint64_t lo;
    uint64_t hi;
};

// Collect, and coalesce adjacent pairs naming the same record — which is what a
// consumer does, and what makes the per-leaf emission below readable as spans.
std::vector<Range> collect(const TreeA& t, uint64_t lo, uint64_t hi) {
    std::vector<Range> out;
    t.enumerate(lo, hi, [&](rdx::Mapping* m, uint64_t a, uint64_t b) {
        if (!out.empty() && out.back().m == m && out.back().hi + 1 == a) {
            out.back().hi = b;
        } else {
            out.push_back(Range{m, a, b});
        }
    });
    return out;
}

}  // namespace

// ─── Order, completeness, and nothing extra ────────────────────────────────

TEST(radix_enumerate_reports_occupied_ranges_in_ascending_order) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));   // C1 root, 1 MiB slots

    // Deliberately inserted out of order, so "ascending" is a property of the
    // enumeration rather than of the insertion sequence.
    const uint64_t offsets[] = {8, 0, 20, 4};
    std::vector<rdx::Mapping*> made;
    for (uint64_t k : offsets) {
        auto* m = makeMapping(k * kPage);
        ASSERT_TRUE(m != nullptr);
        made.push_back(m);
        ASSERT_TRUE(tree.apply(k * kPage, (k + 1) * kPage - 1, m) == rdx::ApplyStatus::Ok);
    }

    const auto got = collect(tree, 0, 64 * kPage - 1);
    ASSERT_EQ(size_t{4}, got.size());
    ASSERT_EQ(uint64_t{0},          got[0].lo);
    ASSERT_EQ(4 * kPage,            got[1].lo);
    ASSERT_EQ(8 * kPage,            got[2].lo);
    ASSERT_EQ(20 * kPage,           got[3].lo);
    for (const auto& r : got) ASSERT_EQ(r.lo + kPage - 1, r.hi);

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("enumerate order");
}

TEST(radix_enumerate_clips_to_the_requested_window) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 16 * kPage - 1, m) == rdx::ApplyStatus::Ok);

    // A window strictly inside the mapping: the reported range is clipped to it,
    // not the mapping's full extent. A consumer iterating a `munmap` span acts
    // on what it is told, so an unclipped report would have it act outside the
    // range it was asked about.
    const auto got = collect(tree, 4 * kPage, 8 * kPage - 1);
    ASSERT_EQ(size_t{1}, got.size());
    ASSERT_EQ(4 * kPage,     got[0].lo);
    ASSERT_EQ(8 * kPage - 1, got[0].hi);

    // An empty window reports nothing rather than misreporting.
    ASSERT_EQ(size_t{0}, collect(tree, 32 * kPage, 40 * kPage - 1).size());

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("enumerate clipping");
}

TEST(radix_enumerate_over_an_empty_cluster_reports_nothing) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(4, 0, h.domain, h.releasePools));
    ASSERT_EQ(size_t{0}, collect(tree, 0, tree.span() - 1).size());
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("enumerate empty");
}

// ─── Chunking: the cursor is a VA, and only a VA ───────────────────────────

// The chunk boundary is a real section close, and the only thing that survives
// it is an address. Driven by stepping chunk-by-chunk and checking the result
// equals the all-at-once answer — a cursor that smuggled a node pointer would
// work here too, so the structural claim is carried by the type (ScanCursor has
// three integers and a flag) and this test carries the RESUMPTION claim.
TEST(radix_enumerate_resumes_across_chunk_boundaries) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(3, 0, h.domain, h.releasePools));   // C0 root, 1 GiB

    // More mappings than one chunk's slot budget, so the enumeration genuinely
    // spans several sections. Placed at C3 granularity inside one C2 node so
    // they are dense rather than scattered.
    constexpr unsigned kCount = 3 * rdx::kScanChunk;
    std::vector<rdx::Mapping*> made;
    for (unsigned k = 0; k < kCount; k++) {
        auto* m = makeMapping(2 * k * kPage);
        ASSERT_TRUE(m != nullptr);
        made.push_back(m);
        ASSERT_TRUE(tree.apply(2 * k * kPage, (2 * k + 1) * kPage - 1, m)
                    == rdx::ApplyStatus::Ok);
    }

    const uint64_t hi = 2 * kCount * kPage;
    const auto whole = collect(tree, 0, hi);
    ASSERT_EQ(size_t{kCount}, whole.size());

    // Now the same walk, chunk by chunk, with the cursor as the only thing
    // crossing each boundary.
    std::vector<Range> stepped;
    auto c = tree.scanFrom(0, hi);
    unsigned chunks = 0;
    while (!c.finished()) {
        tree.enumerateChunk(c, [&](rdx::Mapping* m, uint64_t a, uint64_t b) {
            stepped.push_back(Range{m, a, b});
        });
        chunks++;
        ASSERT_TRUE(chunks < 10000);   // a cursor that never advances is a hang
    }
    // It really did take several sections — otherwise this test is checking the
    // single-chunk path twice.
    ASSERT_TRUE(chunks > 1);

    ASSERT_EQ(whole.size(), stepped.size());
    for (size_t i = 0; i < whole.size(); i++) {
        ASSERT_EQ(whole[i].lo, stepped[i].lo);
        ASSERT_EQ(whole[i].hi, stepped[i].hi);
        ASSERT_EQ(whole[i].m,  stepped[i].m);
    }

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("chunk resumption");
}

// ─── Sparse regions are skipped, not crawled ───────────────────────────────

// The claim with no correctness symptom. An enumeration that advanced one floor
// unit at a time through an empty C0 slot would take 8192 steps per 32 MiB and
// still return the right answer.
//
// Counted in CHUNKS rather than timed: a chunk is a bounded number of slot
// examinations, so "few chunks over a huge sparse span" is exactly the claim,
// and it is deterministic.
TEST(radix_enumerate_skips_sparse_regions_in_whole_slots) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(3, 0, h.domain, h.releasePools));   // C0 root: 32 slots x 32 MiB

    // Two mappings a long way apart, with a 1 GiB-scale hole between them.
    auto* a = makeMapping(0);
    auto* b = makeMapping(30 * rdx::slotSpan(GA, 3));
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_TRUE(tree.apply(0, kPage - 1, a) == rdx::ApplyStatus::Ok);
    ASSERT_TRUE(tree.apply(30 * rdx::slotSpan(GA, 3),
                           30 * rdx::slotSpan(GA, 3) + kPage - 1, b) == rdx::ApplyStatus::Ok);

    auto c = tree.scanFrom(0, tree.span() - 1);
    unsigned chunks = 0;
    size_t emitted = 0;
    while (!c.finished()) {
        emitted += tree.enumerateChunk(c, [](rdx::Mapping*, uint64_t, uint64_t) {});
        chunks++;
        ASSERT_TRUE(chunks < 1000);
    }
    ASSERT_EQ(size_t{2}, emitted);

    // 32 C0 slots plus the descents into the two occupied ones: comfortably one
    // chunk's budget. Crawling the holes at the floor would be ~2.6e5 steps and
    // thousands of chunks, so the margin here is enormous and the bound is not
    // delicate.
    ASSERT_TRUE(chunks <= 2);

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("sparse skip");
}

// A mapping named by several slots is emitted per leaf, in adjacent pieces —
// stated as a test because the alternative (coalescing) would need state that
// outlives a section, and a consumer needs to know which it is getting.
TEST(radix_enumerate_emits_a_multi_slot_mapping_as_adjacent_pieces) {
    Harness h;
    TreeA tree;
    ASSERT_TRUE(tree.init(6, 0, h.domain, h.releasePools));   // C3 root: page-sized slots

    auto* m = makeMapping(0);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(tree.apply(0, 4 * kPage - 1, m) == rdx::ApplyStatus::Ok);
    quiesce(h);
    ASSERT_EQ(uint64_t{4}, m->refcountRelaxed());   // four naming slots

    std::vector<Range> raw;
    tree.enumerate(0, 8 * kPage - 1, [&](rdx::Mapping* mm, uint64_t a, uint64_t b) {
        raw.push_back(Range{mm, a, b});
    });
    ASSERT_EQ(size_t{4}, raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        ASSERT_EQ(m, raw[i].m);
        ASSERT_EQ(i * kPage, raw[i].lo);
        ASSERT_EQ((i + 1) * kPage - 1, raw[i].hi);
    }
    // ...and they coalesce cleanly, which is what makes per-leaf emission a
    // reasonable contract rather than a leak of the tree's shape.
    const auto merged = collect(tree, 0, 8 * kPage - 1);
    ASSERT_EQ(size_t{1}, merged.size());
    ASSERT_EQ(uint64_t{0}, merged[0].lo);
    ASSERT_EQ(4 * kPage - 1, merged[0].hi);

    ASSERT_TRUE(tree.apply(0, tree.span() - 1, nullptr) == rdx::ApplyStatus::Ok);
    quiesce(h);
    tree.destroyTree();
    quiesce(h);
    assertNoLiveObjects("multi-slot emission");
}
