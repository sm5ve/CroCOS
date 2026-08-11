//
// radix-tree — the descent cache (§7.5; DEC-016 / DEC-078 / DEC-079 / DEC-096).
//
// The one feature that crosses the core/policy seam, and the reason the seam
// exports an opaque counted handle at all.
//
// ─── What it is ───────────────────────────────────────────────────────────
//
// A **per-CPU direct-mapped set**, provisionally four entries indexed by VA
// bits, each entry `{address-space generation, VA range, PinnedNode}`. A hit
// short-circuits the walk from the bucket word down to the pinned node; the
// descent resumes from there. Caching the NODE rather than the `Mapping` is what
// makes it a descent shortcut rather than a one-address memo — a hit anywhere in
// the node's span works, and if the indexed slot is a child the descent simply
// carries on down (DEC-016).
//
// Single entry was rejected on the merits: two alternating hot regions thrash it
// deterministically. Fully associative was rejected too — this is the hottest
// path in the kernel and compare cost scales with entries.
//
// ─── Why both a reference and a mark (§7.5's duality) ─────────────────────
//
// Neither mechanism suffices alone and together they are exactly sufficient:
//
//   - the **counted reference** stops the node's memory being freed and reused
//     as a different node. Without it a cached pointer could come to name a
//     valid, unmarked, unrelated node — and the failure is not a crash, it is a
//     `Mapping` returned for the wrong address.
//   - the **dying mark**, acquire-loaded on every hit, tells the entry its node
//     has been detached. **The mark, not the reference, is what makes a cached
//     node's CHILDREN safe to read.** A reference keeps the node alive and says
//     nothing about the slots inside it; for a node that is still live and
//     linked a hit cannot read a freed child, because reclamation clears the
//     parent slot before retiring the child and the hit re-loads that slot in a
//     fresh section. The freed-children hazard is real only for a node that is
//     itself unreachable — detachment and teardown — and both mark.
//
// That is why Phase 3's teardown had to become a marking walk before this file
// could exist: without it, teardown would be the only unlink path setting no
// mark, and a surviving entry would point at a node that is unlinked, unmarked,
// alive, and holding slots that point at freed children.
//
// ─── The install policy, and the trap inside it ───────────────────────────
//
// Install is **VA-range-deferred** (§7.3): a miss records a candidate VA range
// in the missed entry's own register, and a second miss inside that range
// installs during *that* descent's section. Nothing but an address ever crosses
// a section boundary.
//
// The wrong version looks identical and is a silent wrong-answer bug: deferring
// the *reference* — remembering the node pointer from the first miss and
// installing it on the second — holds a raw node pointer across two descents,
// and by the second the recycled slab slot may hold a valid, live, unmarked
// node. The mark check passes and the cache resumes a descent in an unrelated
// part of the tree. **Never defer the reference.**
//
// The register is **per entry**, not one shared register, for the reason the set
// exists: a single shared register never installs under two alternating hot
// regions, which recreates exactly the thrash a direct-mapped set of four is
// there to break.
//
// ─── The eviction is not an optimisation ──────────────────────────────────
//
// A `Detached` or generation-mismatched hit **releases the pin, empties the
// entry, and counts as a miss**. Without that, a hot VA range over a detached
// node pays the generation compare, the mark load *and* the full descent on
// every access forever — strictly worse than no cache, which is §10's own
// design-pressure hazard.
//
// The eviction's release may observe count zero and run `destroy<Node>` **inside
// the descent's open section, in `#PF` context**. That is legal and is no
// exception to any rule: a refcount-zero destroy is the *destructor*, not a
// deleter, so §7.6's deleters-run-outside-any-section rule does not govern it;
// invariant 24 makes the destructor release nothing, so it is bounded
// straight-line work; and vmsmalloc DEC-014 permits `vmsfree` from `#PF`.
//
// ─── Teardown does no cache work ──────────────────────────────────────────
//
// A dead address space's entries are DEC-096's accepted residue — at most
// `processorCount() x entries` nodes per dead address space — released as each
// CPU's cache turns over and made harmless by the generation check. Forcing
// invalidation would need exactly the cross-CPU IPI this project bars from
// correctness duty, so **the generation check is the entire safety story**.
//
// ─── Footprint (a deviation from DEC-079's arithmetic) ────────────────────
//
// DEC-079 records ~192 B per CPU: 4 x {8 B generation, 16 B range, 8 B pin} plus
// 4 x 16 B candidate registers. This implementation is 4 x {8 + 16 + 24} + 4 x 16
// = 256 B, four cache lines rather than three, because **the pin is 24 B and not
// 8**: DEC-012 keeps level and base out of nodes, so a resume cannot recover
// them from the pointer and the handle has to carry them. Packing them back
// (level in the 64 B-alignment slack, base re-derived by masking the key) would
// fit 192 B exactly and was rejected: the derivation is silently wrong for a key
// outside the node, which is the one thing a seam should not be able to be. The
// entry count is DEC-079-Provisional calibration anyway, and the residue bound
// it feeds is stated in NODES, which this does not change.
//

#ifndef CROCOS_RADIX_DESCENT_CACHE_H
#define CROCOS_RADIX_DESCENT_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <kassert.h>
#include <rcu/RCU.h>

#include <mem/radix/AddressSpace.h>
#include <mem/radix/BucketCodec.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/Geometry.h>
#include <mem/radix/Ordering.h>

namespace kernel::mm::radix {

#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 4
    // ─── Mode 4: where does a lookup's cost actually go? ───────────────────
    //
    // Modes 1-3 price a whole `lookup` and the freshness calls inside it. They
    // left a question they cannot answer: mode 2 reported a MINIMUM of 568
    // instructions over a run in which 81% of lookups HIT the descent cache. A
    // hit indexes four entries, compares a range and returns — tens of
    // instructions, not hundreds. So most of a lookup is not the tree walk, and
    // modes 1-3 cannot say what it is.
    //
    // This brackets the two halves of `lookup` separately: `lookupInner` (the
    // cache probe and, on a miss, the descent) against DEC-060's fault-path
    // `tryAdvance` pump, which runs on EVERY lookup and is an epoch load, a
    // conditional O(P) walk over every CPU's reader slot, and an unconditional
    // sweepExpired. The split decides whether a heavy lookup is an algorithm
    // question or a pump-placement one.
    //
    // Lives here rather than in RadixStress.cpp because the seam is inside this
    // function; the stress only prints what these accumulate.
    inline uint64_t probeInsnCounter() noexcept {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // MINIMA for the P4-ITEM-002 reason: interrupts are live in the stress loop,
    // so a timer inside a bracket inflates that sample by the whole handler and
    // the contamination dilutes rather than averaging out.
    inline Atomic<uint64_t> gProbeInnerMin{0};
    inline Atomic<uint64_t> gProbeInnerTicks{0};
    inline Atomic<uint64_t> gProbeInnerCount{0};
    inline Atomic<uint64_t> gProbePumpMin{0};
    inline Atomic<uint64_t> gProbePumpTicks{0};
    inline Atomic<uint64_t> gProbePumpCount{0};
    inline Atomic<uint64_t> gProbeSplitBaseMin{0};

    inline void probeRecordMin(Atomic<uint64_t>& slot, uint64_t sample) noexcept {
        uint64_t cur = slot.load(kCacheAccounting);
        while ((cur == 0 || sample < cur) &&
               !slot.compare_exchange_weak(cur, sample, kCacheAccounting,
                                          kCacheAccounting)) {}
    }
#endif

    // DEC-079, Provisional: "provisionally 4 entries indexed by VA bits". A
    // template parameter rather than a constant because the entry count is
    // explicitly Phase 4/5 calibration and the residue bound of DEC-096 is
    // linear in it — a knob that costs a re-instantiation, not an edit.
    inline constexpr unsigned kDescentCacheEntries = 4;

    // How wide a cache the per-entry instrumentation can describe. Only the
    // SNAPSHOT's array is sized by it — the live per-CPU counters are sized by
    // the instantiation's own `Entries`, since they live inside the template.
    // The snapshot is not, because it is one type shared by every width, and
    // calibration instantiates the cache at several widths in one run: a
    // snapshot that stopped at the default would answer for the default alone.
    inline constexpr unsigned kMaxDescentCacheEntries = 16;

    // How many sightings of a range install it, counting the one that created
    // the candidate. DEC-079 says "a second miss inside that range installs",
    // i.e. two. Runtime rather than compile time because it is the other half of
    // the calibration and, unlike the entry count, it changes no layout.
    //
    // **1 is the un-deferred install DEC-079 rejected** — install on first sight,
    // paying an atomic on a range that may never be seen again — and it is
    // reachable here on purpose, because a knob whose rejected setting cannot be
    // measured is not a calibration input. It is still safe: the pin is taken
    // inside the descent's own section either way, and what defers is the
    // *decision*, not the reference.
    inline constexpr unsigned kInstallThreshold = 2;

    // ─── Instrumentation (the Phase 4 work item) ───────────────────────────
    //
    // The calibration inputs for entry count and install threshold, both
    // Provisional. Counters, never asserts: every one of these is a legal
    // execution and several of them are legal *often*.
    //
    // DEC-016's cost claim is "atomics fall on cache turnover rather than on
    // faults, so cost scales with miss rate", so the atomic counts are broken
    // out per kind — a build where `pinAcquires` tracks `lookups` rather than
    // `installs` has lost the property the design was chosen for.
    //
    // ─── Why one struct template and two instantiations (D-076) ────────────
    //
    // This used to be ONE struct of `Atomic<uint64_t>` hanging off the cache
    // object, incremented with `fetch_add`. That made the instrument the single
    // most contended object in the whole design: `kCacheAccounting` is RELAXED,
    // which removes the fences but **not the lock prefix**, so every increment
    // was a locked read-modify-write taking exclusive ownership of a line every
    // other CPU also wrote. A hit-path lookup bumps three of them. On P CPUs
    // taking page faults, that is 3P locked RMWs per fault-storm on the same
    // three lines — a cost that grows with CPU count on the one path the whole
    // subsystem exists to make fast, and one that D-075's `-icount` method is
    // constitutionally blind to (TCG models no cache, so a locked RMW prices at
    // three instructions there no matter who owns the line).
    //
    // The fix is the layout the rest of this file already uses: the counters
    // move INTO `Row`, which is per-CPU and cache-line aligned, and the
    // increment stops being an RMW. See `DescentCache::bump`.
    //
    // The same field list has to be readable two ways — live (`Atomic<uint64_t>`,
    // written by one CPU, read by any) and as a summed snapshot (plain
    // `uint64_t`, what `stats()` hands back) — so it is written ONCE, here, and
    // instantiated twice. `DescentCacheStats` keeps its name and every field
    // name it had, so the §11 test surface and the stress report are unchanged
    // apart from no longer needing a `.load()`.
    //
    // Field order is deliberate: the three the HIT path bumps come first and
    // adjacent, so a hit dirties one counter line rather than three.
    template <typename T, unsigned N>
    struct CacheCounters {
        // The hit path, in the order `lookup` touches them.
        T lookups{};
        T hits{};
        T freshnessLoads{};

        T misses{};

        // Misses by kind. `emptyEntry` is a cold cache; `rangeMiss` is the entry
        // holding a different range at this index — the thrash signal; the two
        // eviction counts are §7.5's forced-miss cases.
        T emptyEntryMisses{};
        T rangeMisses{};
        T detachedEvictions{};
        T generationEvictions{};
        // A resume that the seam refused because the key was outside the pinned
        // node. Unreachable through this cache — the entry's VA range is the
        // node's span — and counted rather than asserted so a future caller of
        // the seam that gets it wrong shows up as a number instead of silence.
        T outOfRangeResumes{};

        // Install machinery.
        T candidatesRecorded{};
        T candidateSightings{};
        T installs{};
        // Installs that displaced a live entry. The other half of the thrash
        // picture: high `rangeMisses` with high `installEvictions` is two hot
        // regions fighting over one index, which is what more entries buys.
        T installEvictions{};
        // Eviction releases that took a node's count to zero and destroyed it —
        // §7.5's in-section, `#PF`-context `destroy<Node>`. The residue of
        // DEC-096 coming home.
        T releasesThatDestroyed{};

        // Atomics this cache performed, by kind.
        T pinAcquires{};
        T pinReleases{};

        // Per-entry thrash: a miss at an index whose entry held a different
        // range. Per index rather than summed, because the question calibration
        // asks is whether the pressure is spread or concentrated.
        T entryThrash[N]{};
    };

    // The number of scalar counters above, i.e. everything but `entryThrash`.
    // Named because two things are checked against it and both are silent
    // failures otherwise — see the static_assert below and `zipCounterScalars`.
    inline constexpr unsigned kCacheScalarCounters = 16;

    // What `stats()` returns: every CPU's row summed, as plain integers.
    using DescentCacheStats = CacheCounters<uint64_t, kMaxDescentCacheEntries>;

    // The forcing function for the one staleness hazard this shape introduces.
    // `zipCounterScalars` below enumerates the scalar counters by name, and a
    // counter added to the struct but not to the zip would simply never appear
    // in a snapshot — a number that reads as zero rather than as a build error,
    // which is the failure mode R-4 caught in the ordering scanner's header
    // list. Adding a field changes this size and breaks the build instead.
    static_assert(sizeof(DescentCacheStats) ==
                      (kCacheScalarCounters + kMaxDescentCacheEntries) * sizeof(uint64_t),
                  "a counter was added to or removed from CacheCounters — update "
                  "kCacheScalarCounters AND zipCounterScalars, or the new counter will "
                  "silently read as zero in every snapshot");

    // The ONE place the counter names are enumerated outside the declaration.
    // Both directions of traffic run through it — summing a CPU's row into a
    // snapshot, and clearing a row — so the two cannot drift apart. `entryThrash`
    // is deliberately NOT here: the two sides have different extents (a snapshot
    // is `kMaxDescentCacheEntries` wide, a row is `Entries` wide), so its loop
    // belongs to the caller that knows both.
    //
    // Untyped in both arguments so one template serves `(snapshot, const row)`
    // and `(row, row)`; the static_assert above is what keeps it honest.
    template <typename A, typename B, typename F>
    inline void zipCounterScalars(A& a, B& b, F&& f) {
        f(a.lookups, b.lookups);
        f(a.hits, b.hits);
        f(a.freshnessLoads, b.freshnessLoads);
        f(a.misses, b.misses);
        f(a.emptyEntryMisses, b.emptyEntryMisses);
        f(a.rangeMisses, b.rangeMisses);
        f(a.detachedEvictions, b.detachedEvictions);
        f(a.generationEvictions, b.generationEvictions);
        f(a.outOfRangeResumes, b.outOfRangeResumes);
        f(a.candidatesRecorded, b.candidatesRecorded);
        f(a.candidateSightings, b.candidateSightings);
        f(a.installs, b.installs);
        f(a.installEvictions, b.installEvictions);
        f(a.releasesThatDestroyed, b.releasesThatDestroyed);
        f(a.pinAcquires, b.pinAcquires);
        f(a.pinReleases, b.pinReleases);
    }

    // ─── The cache ─────────────────────────────────────────────────────────
    //
    // **Machine-global and per-CPU**, not per-address-space, and the generation
    // check is what makes that safe: an entry left behind by a dead address
    // space cannot be mistaken for a live one, because the generation is a
    // monotonic counter that is never recycled (DEC-082) — so a recycled control
    // block address cannot alias a dead address space.
    //
    // Per-CPU rather than per-thread mirrors the pinned-writer/faulting-CPU
    // discipline vmsmalloc's arenas already established. Each CPU touches only
    // its own row, so the row's fields are plain — **including the counters**
    // (D-076). They are `Atomic` only because `stats()` reads them from a
    // foreign CPU and a plain word written here and read there is a data race
    // TSan is right to flag; nothing about them is shared-write, and nothing
    // reads them for a correctness decision.
    //
    // There is therefore NO shared mutable state on this object at all. That is
    // a property worth stating, because it is what makes a page fault on CPU 7
    // generate no coherence traffic against CPU 3's fault.
    template <GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget,
              unsigned Entries = kDescentCacheEntries>
    class DescentCache {
    public:
        using Tree  = CoreTree<G, Codec, DetachBudget>;
        using Block = ControlBlock<G, Codec, DetachBudget>;
        using Pin   = PinnedNode<G>;

        static_assert(Entries > 0 && (Entries & (Entries - 1)) == 0,
                      "the descent cache is DIRECT-MAPPED: the entry count must be a power "
                      "of two, or the VA-bit index is not a mask");
        static_assert(Entries <= kMaxDescentCacheEntries,
                      "DescentCacheStats::entryThrash is sized at kMaxDescentCacheEntries; "
                      "grow that constant before instantiating a wider cache");

        // `pinLevel` is DEC-078's caller-chosen level. The default —
        // `kPinAtDeepest` — is DEC-016's "interior node directly above the last
        // accessed mapping", which is what a fault path wants: the shortest
        // remaining descent. A numeric level exists for the §11 targets that
        // must drive an *interior* entry deliberately, and for calibration.
        //
        // The index shift follows the pin level rather than being a constant,
        // because indexing finer than the pinned node's span installs the same
        // node into several entries and indexing coarser makes two distinct
        // pinnable nodes collide. Both are correct; both waste the set.
        explicit DescentCache(unsigned pinLevel = Tree::kPinAtDeepest,
                              unsigned installThreshold = kInstallThreshold)
            : pinAtLevel(pinLevel), threshold(installThreshold),
              indexShift(nodeSpanBits(G, pinLevel == Tree::kPinAtDeepest ? G.levelCount
                                                                         : pinLevel)) {
            assert(installThreshold >= 1,
                   "radix cache: the install threshold counts sightings and so starts at 1 "
                   "(which is itself the un-deferred install DEC-079 rejected)");
        }

        DescentCache(const DescentCache&)            = delete;
        DescentCache& operator=(const DescentCache&) = delete;

        // Every CPU's row, summed. **By value, and that is the interface** — the
        // counters no longer exist as one object that could be handed out by
        // reference, and a caller that wants two of them consistent with each
        // other must take one snapshot and read both fields from it.
        //
        // Two honesty notes that the old shared struct also owed but never said:
        // the sum is not an instant, since the rows are read one after another;
        // and a row can be written while it is being read. Neither matters for
        // what these are for — "a hit rate computed from two counters sampled a
        // nanosecond apart is still a hit rate" (`kCacheAccounting`) — and every
        // §11 assertion that wants an EXACT number takes it on a quiesced cache.
        //
        // Summed over the whole array rather than `arch::processorCount()`: rows
        // are indexed by logical processor ID with no bound but the array's, so
        // the array is the only bound that cannot be wrong. It costs a few
        // thousand relaxed loads on a path that is a report, never a fault.
        [[nodiscard]] DescentCacheStats stats() const {
            DescentCacheStats s{};
            for (size_t c = 0; c < arch::MAX_PROCESSOR_COUNT; c++) {
                const Counters& k = rows[c].counters;
                zipCounterScalars(s, k, [](uint64_t& dst, const Atomic<uint64_t>& src) {
                    dst += src.load(kCacheAccounting);
                });
                for (unsigned i = 0; i < Entries; i++) {
                    s.entryThrash[i] += k.entryThrash[i].load(kCacheAccounting);
                }
            }
            return s;
        }

        // Zero every CPU's row. Calibration wants a clean baseline between
        // sweeps; nothing on the fault path calls it.
        void resetStats() {
            for (size_t c = 0; c < arch::MAX_PROCESSOR_COUNT; c++) {
                Counters& k = rows[c].counters;
                // Same row on both sides: the zip exists to name the counters
                // once, and clearing needs only one of the pair.
                zipCounterScalars(k, k, [](Atomic<uint64_t>& dst, Atomic<uint64_t>&) {
                    dst.store(0, kCacheAccounting);
                });
                for (unsigned i = 0; i < Entries; i++) {
                    k.entryThrash[i].store(0, kCacheAccounting);
                }
            }
        }

        // ─── The lookup ────────────────────────────────────────────────────
        //
        // The whole feature. Reading it top to bottom is the specification:
        // generation, then mark, then descend on a hit; candidate then install
        // on a miss; and every failure path falls through to a full descent
        // rather than inventing an answer.
        [[nodiscard]] LookupResult lookup(Block& block, uint64_t va) {
            // The `lookups` bump lives at the top of `lookupInner` rather than
            // here, because the counter is now in the row and finding the row
            // costs an out-of-line `arch::getCurrentProcessorID()` — doing it
            // twice per lookup to keep one increment outside the probe bracket
            // would cost more than the increment.
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 4
            // The probe pairs are the only thing between the two brackets, so
            // each half is stated net of `gProbeSplitBaseMin`.
            const uint64_t b0 = probeInsnCounter();
            const uint64_t b1 = probeInsnCounter();
            probeRecordMin(gProbeSplitBaseMin, b1 - b0);

            const uint64_t i0 = probeInsnCounter();
            LookupResult r = lookupInner(block, va);
            const uint64_t i1 = probeInsnCounter();
            probeRecordMin(gProbeInnerMin, i1 - i0);
            gProbeInnerTicks.fetch_add(i1 - i0, kCacheAccounting);
            gProbeInnerCount.fetch_add(1, kCacheAccounting);

            const uint64_t p0 = probeInsnCounter();
            (void)kernel::rcu::pumpIfWork(block.domain);
            const uint64_t p1 = probeInsnCounter();
            probeRecordMin(gProbePumpMin, p1 - p0);
            gProbePumpTicks.fetch_add(p1 - p0, kCacheAccounting);
            gProbePumpCount.fetch_add(1, kCacheAccounting);
            return r;
#else
            LookupResult r = lookupInner(block, va);

            // DEC-060's fault-path pump, after every section this call opened has
            // closed. Placement is forced, not stylistic: deleters run outside
            // any section (RCU-DEC-038), and a pump inside a descent's section
            // could run deleters while link-loaded pointers are still live.
            // Gated on sealed-bag population (pumpIfWork): the D-076 probe put
            // the unconditional pump's sweep at 0-of-545 productive at 2 CPUs,
            // so the common case is one relaxed load and nothing else.
            (void)kernel::rcu::pumpIfWork(block.domain);
            return r;
#endif
        }

        // Drop everything this CPU holds. Not part of the protocol — teardown
        // deliberately does no cache work — but the harness needs a way to
        // return the residue deterministically, and so does a CPU going offline.
        void evictAllOnThisCpu() {
            Row& row = rowForThisCpu();
            for (unsigned i = 0; i < Entries; i++) {
                evict(row.counters, row.entries[i]);
                row.candidates[i] = Candidate{};
            }
        }

        // Introspection for the §11 targets, deliberately NOT a way to reach the
        // handle: a caller can ask what an entry covers and whether it is
        // occupied, never what node it names. The policy layer never
        // dereferences the handle, and neither does a test.
        [[nodiscard]] bool entryOccupied(unsigned index) const {
            assert(index < Entries, "radix cache: entry index out of range");
            return static_cast<bool>(rowForThisCpu().entries[index].pin);
        }
        [[nodiscard]] uint64_t entryGeneration(unsigned index) const {
            assert(index < Entries, "radix cache: entry index out of range");
            return rowForThisCpu().entries[index].generation;
        }
        [[nodiscard]] uint64_t entryLo(unsigned index) const {
            assert(index < Entries, "radix cache: entry index out of range");
            return rowForThisCpu().entries[index].lo;
        }
        [[nodiscard]] uint64_t entryHi(unsigned index) const {
            assert(index < Entries, "radix cache: entry index out of range");
            return rowForThisCpu().entries[index].hi;
        }
        [[nodiscard]] unsigned indexFor(uint64_t va) const {
            return static_cast<unsigned>((va >> indexShift) & (Entries - 1));
        }

    private:
        struct Entry {
            // Zero is the empty generation: DEC-082 assigns generations from 1
            // and never recycles them, so there is no live address space this
            // can collide with.
            uint64_t generation = 0;
            uint64_t lo = 0;
            uint64_t hi = 0;    // inclusive; the pinned node's whole span
            Pin      pin{};
        };

        // §7.5's per-entry candidate register. A VA RANGE and a sighting count —
        // never a node pointer, which is the whole point.
        struct Candidate {
            uint64_t lo = 0;
            uint64_t hi = 0;
            unsigned sightings = 0;   // 0 == no candidate
        };

        // The live counters. Sized by this instantiation's `Entries`, not by
        // `kMaxDescentCacheEntries` — only the snapshot has to answer for every
        // width, and a row that carried twelve unused words would be twelve
        // words of every CPU's working set.
        using Counters = CacheCounters<Atomic<uint64_t>, Entries>;

        // `alignas(CACHE_LINE_SIZE)` is what stops CPU 3's counters and CPU 4's
        // entries sharing a line — the false sharing that would put back, in a
        // subtler place, exactly the coherence traffic moving the counters here
        // removed. It was already required for `entries`; it now covers more.
        struct alignas(arch::CACHE_LINE_SIZE) Row {
            Entry     entries[Entries];
            Candidate candidates[Entries];
            Counters  counters{};
        };

        [[nodiscard]] Row& rowForThisCpu() {
            return rows[static_cast<size_t>(arch::getCurrentProcessorID())];
        }
        [[nodiscard]] const Row& rowForThisCpu() const {
            return rows[static_cast<size_t>(arch::getCurrentProcessorID())];
        }

        // A counter increment on a row **only this CPU writes**.
        //
        // Not `fetch_add`, and the difference is the whole point of D-076: a
        // locked read-modify-write is what makes an increment cost a cache line
        // instead of a register, and there is nothing for it to arbitrate here.
        // One writer cannot lose an update to itself, so load/add/store says
        // exactly as much and compiles to an unlocked `inc` on a line this CPU
        // already owns.
        //
        // `Atomic` rather than plain `uint64_t` because `stats()` reads these
        // from a FOREIGN CPU. A plain word written here and read there is a data
        // race — undefined by the standard and flagged by TSan, which is this
        // project's default release gate — and relaxed atomics are the cheapest
        // spelling that is not one. They also cost nothing extra: on both
        // targets a relaxed 64-bit load and store are a plain load and store.
        static void bump(Atomic<uint64_t>& c) {
#if defined(CROCOS_RADIX_BILL_NOBUMP)   // measurement scaffold — never commit enabled
            (void)c;
#else
            c.store(c.load(kCacheAccounting) + 1, kCacheAccounting);
#endif
        }

        // The one place a pin is released. Counted here rather than at the call
        // sites so no eviction path can forget to account for its atomic.
        void evict(Counters& counters, Entry& e) {
            if (!e.pin) { e = Entry{}; return; }
            bump(counters.pinReleases);
            // A zero-observing release DESTROYS, and this is the site §7.5 says
            // may do so inside an open section in `#PF` context — legal because
            // it is the destructor and not a deleter. Counted, because that is
            // DEC-096's residue actually coming home and DEC-016's cost claim is
            // stated in exactly these atomics.
            if (e.pin.release()) {
                bump(counters.releasesThatDestroyed);
            }
            e = Entry{};
        }

        [[nodiscard]] LookupResult lookupInner(Block& block, uint64_t va) {
            Row&        row  = rowForThisCpu();
            Counters&   ctr  = row.counters;
            const unsigned i = indexFor(va);
            Entry&      e    = row.entries[i];
            Candidate&  cand = row.candidates[i];
            const uint64_t generation = block.generation;

            bump(ctr.lookups);

            // The cluster this VA belongs to. A bucket index is a pure prefix of
            // the VA (DEC-033), so this needs no load and cannot be stale — which
            // is exactly why DEC-091's per-CPU cell holds an index and never a
            // decoded pointer.
            Tree tree = block.clusters.treeFor(bucketIndexFor<G>(va));

            // ─── Hit ───────────────────────────────────────────────────────
            //
            // Generation, then mark, then descend. The order is §7.5's and it is
            // load-bearing: the generation compare is what makes a dead address
            // space's residue harmless, and doing the mark load first would be
            // loading the state word of a node belonging to a process that no
            // longer exists.
            if (e.pin && va >= e.lo && va <= e.hi) {
                if (e.generation == generation) {
                    bump(ctr.freshnessLoads);
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
                    // The whole resume, so the three sub-brackets in CoreTree.h
                    // can be stated as a fraction of it and the residue
                    // attributed to this function's own preamble and tail.
                    const uint64_t rs0 = probe5Counter();
#endif
                    auto resumed = tree.resumeDescent(e.pin, va);
#if defined(CROCOS_RADIX_INSN_PROBE) && CROCOS_RADIX_INSN_PROBE == 5
                    const uint64_t rs1 = probe5Counter();
                    probe5RecordMin(gProbe5ResumeMin, rs1 - rs0);
#endif
                    if (resumed.status == Tree::ResumeStatus::Ok) {
                        bump(ctr.hits);
                        return static_cast<LookupResult&&>(resumed.result);
                    }
                    if (resumed.status == Tree::ResumeStatus::OutOfRange) {
                        bump(ctr.outOfRangeResumes);
                    } else {
                        bump(ctr.detachedEvictions);
                    }
                    evict(ctr, e);
                } else {
                    bump(ctr.generationEvictions);
                    evict(ctr, e);
                }
            } else if (e.pin) {
                // Occupied, but by a different range at this index. The thrash
                // signal the entry count is calibrated against.
                bump(ctr.rangeMisses);
                bump(ctr.entryThrash[i]);
            } else {
                bump(ctr.emptyEntryMisses);
            }
            bump(ctr.misses);

            // ─── Miss: the VA-range-deferred install ───────────────────────
            //
            // The install decision is taken BEFORE the descent, from the
            // candidate register alone — an address, and nothing else, has
            // crossed from the earlier section. What the descent then pins is
            // whatever is there NOW, re-found from the root. That is the whole
            // difference between this and the recycled-slot ABA of §7.3.
            const bool inCandidate =
                cand.sightings > 0 && va >= cand.lo && va <= cand.hi;
            // Threshold 1 is install-on-first-sight and needs no candidate at
            // all; every other value counts sightings of one range, the
            // candidate-creating miss included.
            const bool install =
                threshold <= 1 || (inCandidate && (cand.sightings + 1) >= threshold);

            LookupResult result;
            typename Tree::PinSite site;
            Pin      pin;
            uint64_t nodeLo = 0, nodeHi = 0;
            {
                kernel::rcu::ReadGuard guard(block.domain);
                result = tree.descendLocked(tree.currentBinding(), va, pinAtLevel, &site);
                if (site.valid) {
                    nodeLo = site.base;
                    nodeHi = site.base + Geo<G>::nodeSpan(site.level) - 1;
                    if (install) {
                        // §7.3's acquisition law: inside the section that
                        // observed the link, which is this one.
                        bump(ctr.pinAcquires);
                        pin = tree.pinLocked(site);
                        // ...and the eviction it forces happens here too, so the
                        // zero-observing destroy runs inside this descent's open
                        // section. §7.5 says that is legal; the harness exercises
                        // it deliberately rather than letting it happen by
                        // accident on some future path.
                        if (e.pin) bump(ctr.installEvictions);
                        evict(ctr, e);
                        e.pin        = static_cast<Pin&&>(pin);
                        e.generation = generation;
                        e.lo         = nodeLo;
                        e.hi         = nodeHi;
                        cand = Candidate{};
                        bump(ctr.installs);
                    }
                }
            }

            if (!install && site.valid) {
                if (inCandidate) {
                    cand.sightings++;
                    bump(ctr.candidateSightings);
                } else {
                    // A fresh candidate. Recorded even when the entry is
                    // occupied by someone else's range: that is precisely the
                    // alternating-regions case, and refusing to re-candidate
                    // would freeze the loser out permanently.
                    cand.lo = nodeLo;
                    cand.hi = nodeHi;
                    cand.sightings = 1;
                    bump(ctr.candidatesRecorded);
                }
            }
            return result;
        }

        const unsigned pinAtLevel;
        const unsigned threshold;
        const unsigned indexShift;

        // The whole of this cache's mutable state, and every byte of it is
        // owned by exactly one CPU.
        Row rows[arch::MAX_PROCESSOR_COUNT]{};
    };

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_DESCENT_CACHE_H
