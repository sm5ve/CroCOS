//
// radix-tree — the quiesced-tree validator (§8 invariants; §11 targets).
//
// Everything checkable by walking a tree nobody is mutating. §11 is explicit
// that some of these must NOT be asserted mid-operation — occupancy is exact
// only when no actor other than the reader holds a claim bit — so this is a
// between-operations validator, and Phase 2 must keep it that way rather than
// moving its checks inline.
//
// The two checks worth naming, because both have a history of passing while a
// real defect was present:
//
//   - **"No node marked but linked"** is the ONLY check that catches an
//     operation which marked a subtree and then backed off. Nothing else sees
//     it: tree shape, the partition invariant and hole-freedom all still hold,
//     and the range is simply, silently, unmappable for the address space's
//     life.
//
//   - **Mapping count vs naming slots** is the N:1 rule, and it must be
//     asserted in BOTH directions. A leak and a double-release are opposite
//     failures of one rule, and a check that only detects "too high" passes a
//     premature free. It must also run with NO outstanding lookup reference,
//     which would otherwise hold every count above its structural value and
//     mask the whole thing.
//

#ifndef CROCOS_RADIX_TREE_VALIDATOR_H
#define CROCOS_RADIX_TREE_VALIDATOR_H

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <cstdio>

#include <mem/radix/Geometry.h>
#include <mem/radix/Node.h>
#include <mem/radix/CoreTree.h>

#include <assert_support.h>

namespace CroCOSTest::radix {

    namespace rdx = kernel::mm::radix;

    template <rdx::GeometryDescriptor G, typename Codec>
    class TreeValidator {
    public:
        // Walks the tree and checks every structural and lifetime invariant that
        // is decidable from shape alone. Throws on the first violation, naming
        // the invariant.
        template <unsigned DB>
        static void validate(const rdx::CoreTree<G, Codec, DB>& tree, const char* what) {
            TreeValidator v(what);
            if (!tree.valid()) return;
            v.visit(tree.root(), tree.rootLevel(), tree.base(), /*isRoot=*/true);
            v.checkMappingCounts();
        }

        // The naming-slot census on its own, for tests that want the numbers.
        template <unsigned DB>
        static std::map<const rdx::Mapping*, size_t>
        namingSlotCensus(const rdx::CoreTree<G, Codec, DB>& tree) {
            TreeValidator v("census");
            if (tree.valid()) v.visit(tree.root(), tree.rootLevel(), tree.base(), true);
            return v.namingSlots;
        }

    private:
        explicit TreeValidator(const char* w) : what(w) {}

        const char* what;
        std::map<const rdx::Mapping*, size_t> namingSlots;

        [[noreturn]] void fail(const char* invariant, uint64_t where) const {
            char msg[320];
            std::snprintf(msg, sizeof(msg), "%s: %s (at VA %llu)",
                          what, invariant, static_cast<unsigned long long>(where));
            throw AssertionFailure(std::string(msg));
        }

        void visit(rdx::NodeRef node, unsigned level, uint64_t nodeBase, bool isRoot) {
            const unsigned n = rdx::valence(G, level);
            const uint64_t span = rdx::slotSpan(G, level);
            const uint64_t word = node.stateWord().load(RELAXED);

            // §11: "No node is left marked but linked." Reached from the root,
            // so by definition linked.
            if (rdx::state::isMarked(word)) {
                fail("invariant: a node reachable from the root carries the dying mark — "
                     "an operation marked a subtree and then backed off, which leaves the "
                     "range permanently unmappable with no other symptom", nodeBase);
            }

            // §8 invariant 12: occupancy is exact whenever no other actor holds
            // a claim bit. On a quiesced tree that is unconditional.
            unsigned occupied = 0;
            for (unsigned i = 0; i < n; i++) {
                if (!Codec::isEmpty(node.slot(i).load(RELAXED))) occupied++;
            }
            if (rdx::state::countOf(word) != occupied) {
                char detail[192];
                std::snprintf(detail, sizeof(detail),
                              "invariant 12: occupancy count %u != non-empty slot count %u",
                              rdx::state::countOf(word), occupied);
                fail(detail, nodeBase);
            }

            // The spare bits are held at zero — which is what makes an
            // occupancy underflow's borrow DETECTABLE at all, since it sets
            // bits 39..63 rather than vanishing.
            if (rdx::state::spareOf(word) != 0) {
                fail("invariant: state-word spare bits are non-zero — an occupancy borrow "
                     "escaped the count field", nodeBase);
            }

            // A quiesced tree holds no claims.
            if (rdx::state::claimsOf(word) != 0) {
                fail("invariant: claim bits held on a quiesced tree", nodeBase);
            }

            // §6.4 / §11 "reclamation actually reclaims": an empty interior
            // node must not survive. The cluster root is the one exemption —
            // the walk terminates there and it is never a candidate, because
            // its parent slot is a bucket word with no state word to acquire
            // the interlock on, and reclaiming it anyway would leave that word
            // naming freed memory: an entire zone permanently unmappable with
            // no crash.
            //
            // This is asserted DIRECTLY rather than inferred from a memory
            // figure, because the historical failure here was deterministic and
            // invisible to every structural check — the partition invariant
            // held, and a "no node marked but linked" validator passed
            // precisely because no mark was ever set.
            if (occupied == 0 && !isRoot) {
                fail("§6.4: an empty non-root node survived — reclamation did not reclaim, "
                     "and with random-probe placement a freed region is essentially never "
                     "reused, so nothing else ever will", nodeBase);
            }

            for (unsigned i = 0; i < n; i++) {
                const uint64_t w = node.slot(i).load(RELAXED);
                const uint64_t slotBase = nodeBase + uint64_t{i} * span;

                switch (Codec::kindOf(w)) {
                case rdx::SlotKind::Empty:
                    break;

                case rdx::SlotKind::Child: {
                    // §8 invariant 4: an encoded child is nonzero and untagged,
                    // guaranteed at any allocation offset by kChildGuardBit.
                    if ((w & rdx::kChildGuardBit) == 0) {
                        fail("invariant 4: an encoded child word is missing kChildGuardBit — "
                             "a node at window offset 0 would encode to all-zero and read as "
                             "an EMPTY SLOT", slotBase);
                    }
                    if (level >= G.levelCount) {
                        fail("invariant: child pointer at the deepest level", slotBase);
                    }
                    // Round-trip, on the live word.
                    void* child = Codec::decodeChild(w);
                    if (Codec::encodeChild(child) != w) {
                        fail("invariant 4: child word does not round-trip", slotBase);
                    }
                    visit(rdx::NodeRef(child), level + 1, slotBase, /*isRoot=*/false);
                    break;
                }

                case rdx::SlotKind::Leaf: {
                    const rdx::SubRange r = Codec::decodeRange(w, level);
                    // §8 invariant 3: a leaf's covered span is a contiguous
                    // sub-range of its slot's naturally-aligned span. Both
                    // halves matter — the inclusive-end encoding cannot express
                    // an empty range, so lo > hi would mean a corrupted word.
                    if (r.lo > r.hi) {
                        fail("invariant 3: leaf sub-range is empty (lo > hi)", slotBase);
                    }
                    if (r.hi >= rdx::rangeUnitCount(G, level)) {
                        fail("invariant 3: leaf sub-range escapes its slot span", slotBase);
                    }
                    auto* m = static_cast<rdx::Mapping*>(Codec::decodeLeaf(w));
                    if (Codec::encodeLeaf(m, r, level) != w) {
                        fail("invariant 4: leaf word does not round-trip", slotBase);
                    }
                    namingSlots[m]++;
                    break;
                }
                }
            }
        }

        void checkMappingCounts() {
            for (const auto& [m, slots] : namingSlots) {
                const uint64_t actual = m->refcountRelaxed();
                if (actual != slots) {
                    char detail[256];
                    std::snprintf(detail, sizeof(detail),
                                  "§7.2: Mapping %p has structural count %llu but is named by "
                                  "%zu leaf slots — %s",
                                  static_cast<const void*>(m),
                                  static_cast<unsigned long long>(actual), slots,
                                  actual > slots
                                      ? "a leak (or an outstanding lookup reference the test "
                                        "must not be holding)"
                                      : "a DOUBLE RELEASE — the count will reach zero with a "
                                        "live naming slot");
                    throw AssertionFailure(std::string(detail));
                }
            }
        }
    };

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_TREE_VALIDATOR_H
