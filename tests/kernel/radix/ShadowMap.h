//
// radix-tree — the serialized reference implementation (§12 requirement 1).
//
// "Leaves-at-any-level means 'is this tree correct?' is no longer answerable by
// inspection, and sub-range cover means the model must include the codec."
//
// A flat array of one entry per resolution-floor unit over the tree's span: the
// dumbest possible partition model, which is exactly the property that makes it
// trustworthy as an oracle. Every operation the tree supports has a two-line
// implementation here, and any disagreement is the tree's.
//
// ─── What the model can and cannot predict ─────────────────────────────────
//
// It predicts COVER — for each address, which record (if any) is mapped there.
// It deliberately does NOT predict the tree's reported *range*, because that is
// a function of tree shape rather than of semantics: a 192 KiB mapping across
// three C2 slots is three leaves reporting three 64 KiB ranges, not one 192 KiB
// range, and both are correct. So the range is checked by a weaker but still
// sharp property — the reported range must contain the queried address and must
// be uniform in the model — which catches an over-wide leaf (the hole-freedom
// violation) without encoding tree shape into the oracle.
//
// It also cannot see TRANSLATION (§5.4's `objectOffset + (faultVA - baseVA)`),
// which is why that gets its own test rather than riding the model check.
//

#ifndef CROCOS_RADIX_SHADOW_MAP_H
#define CROCOS_RADIX_SHADOW_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <cstdio>

#include <mem/radix/Geometry.h>
#include <mem/radix/Mapping.h>
#include <mem/radix/CoreTree.h>

#include <assert_support.h>

namespace CroCOSTest::radix {

    namespace rdx = kernel::mm::radix;

    template <rdx::GeometryDescriptor G>
    class ShadowMap {
    public:
        ShadowMap(unsigned rootLevel, uint64_t base)
            : baseVA(base),
              unitBits(G.floorBits),
              units(rdx::nodeSpan(G, rootLevel) >> G.floorBits),
              cells(static_cast<size_t>(units), nullptr) {}

        void apply(uint64_t lo, uint64_t hi, rdx::Mapping* value) {
            const size_t first = unitOf(lo);
            const size_t last  = unitOf(hi);
            for (size_t u = first; u <= last; u++) cells[u] = value;
        }

        [[nodiscard]] rdx::Mapping* at(uint64_t va) const { return cells[unitOf(va)]; }

        // The maximal run of the same record containing `va`. The tree's
        // reported range must be a sub-range of this.
        void runAt(uint64_t va, uint64_t& lo, uint64_t& hi) const {
            const size_t u = unitOf(va);
            rdx::Mapping* m = cells[u];
            size_t a = u, b = u;
            while (a > 0 && cells[a - 1] == m) a--;
            while (b + 1 < cells.size() && cells[b + 1] == m) b++;
            lo = baseVA + (static_cast<uint64_t>(a) << unitBits);
            hi = baseVA + ((static_cast<uint64_t>(b) + 1) << unitBits) - 1;
        }

        [[nodiscard]] size_t mappedUnits() const {
            size_t n = 0;
            for (auto* c : cells) if (c) n++;
            return n;
        }

        // How many floor units name `m` — the ground truth a Mapping's
        // structural count must be consistent with (though NOT equal to: the
        // count is per naming SLOT, and one slot covers many units).
        [[nodiscard]] size_t unitsNaming(const rdx::Mapping* m) const {
            size_t n = 0;
            for (auto* c : cells) if (c == m) n++;
            return n;
        }

        [[nodiscard]] uint64_t unitCount() const { return units; }
        [[nodiscard]] uint64_t base() const { return baseVA; }

    private:
        size_t unitOf(uint64_t va) const {
            return static_cast<size_t>((va - baseVA) >> unitBits);
        }

        uint64_t baseVA;
        unsigned unitBits;
        uint64_t units;
        std::vector<rdx::Mapping*> cells;
    };

    // ─── Cross-check ───────────────────────────────────────────────────────
    //
    // Compares a tree against its model at EVERY floor unit. Cheap on the tiny
    // geometry (256 units), which is what makes the exhaustive sweep possible;
    // on the real geometry the caller samples instead.
    template <rdx::GeometryDescriptor G, typename Codec, unsigned DB>
    void assertMatchesModel(const rdx::CoreTree<G, Codec, DB>& tree,
                            const ShadowMap<G>& model,
                            const char* what) {
        const uint64_t unitSize = uint64_t{1} << G.floorBits;
        for (uint64_t u = 0; u < model.unitCount(); u++) {
            const uint64_t va = model.base() + u * unitSize;
            const auto got = tree.lookup(va);
            rdx::Mapping* want = model.at(va);

            if (got.mapping() != want) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "%s: cover mismatch at VA %llu — tree says %p, model says %p",
                              what, static_cast<unsigned long long>(va),
                              static_cast<const void*>(got.mapping().address()),
                              static_cast<const void*>(want));
                throw AssertionFailure(std::string(msg));
            }
            if (!want) continue;

            // The reported range must contain the query and be uniform in the
            // model. An over-wide leaf — the hole-freedom violation, and the one
            // a cover-only check would miss — fails the uniformity half.
            if (got.lo() > va || got.hi() < va) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "%s: reported range [%llu,%llu] does not contain VA %llu",
                              what,
                              static_cast<unsigned long long>(got.lo()),
                              static_cast<unsigned long long>(got.hi()),
                              static_cast<unsigned long long>(va));
                throw AssertionFailure(std::string(msg));
            }
            uint64_t runLo = 0, runHi = 0;
            model.runAt(va, runLo, runHi);
            if (got.lo() < runLo || got.hi() > runHi) {
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                              "%s: reported range [%llu,%llu] at VA %llu escapes the model's "
                              "uniform run [%llu,%llu] — the leaf covers addresses that do not "
                              "name this record",
                              what,
                              static_cast<unsigned long long>(got.lo()),
                              static_cast<unsigned long long>(got.hi()),
                              static_cast<unsigned long long>(va),
                              static_cast<unsigned long long>(runLo),
                              static_cast<unsigned long long>(runHi));
                throw AssertionFailure(std::string(msg));
            }
        }
    }

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_SHADOW_MAP_H
