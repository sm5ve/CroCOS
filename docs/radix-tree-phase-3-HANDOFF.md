# radix-tree — implementation handoff into Phase 3

**Written 2026-08-08 at `e3b82bd`; UPDATED 2026-08-08 at `ea364af`.** Branch
`radix-tree`. Phases 0, 1 and 2 are complete and green, and **four of Phase 3's
nine work items have landed** — see §0, which is the only part of this note that
has moved, and which also carries the binding rule growth turns on. Everything below §0 was written before Phase 3 started and still
holds except where §0 says otherwise.

Read in this order:

1. §0 of this note, then the rest of it.
2. `specs/radix-tree-phase-3.md` — the work items and exit gate; the landed ones
   are ticked.
3. `docs/radix-tree-implementation-deviations.md` — D-001..D-029, the running
   findings log. **D-004, D-010 and D-029 are the live obligations now**;
   D-003 and D-011 are closed (see §0).
4. `specs/radix-tree.md` §3.1, §5.1/5.4/5.6, §7 entire, §9, §11 — Phase 3's
   sections. `specs/radix-tree-HANDOFF.md` is the *spec*-side note and is only
   needed if you intend to edit the spec.

---

## 0. Phase 3 so far

### Landed (all committed, all green)

| Commit | Item |
|---|---|
| `313a2ae` | **`vmsmallocTry` / `tryMake` kernel-side** (DEC-048). Closes D-003. |
| `8a5b1f7` | **`DeferredRelease` pools** (§7.1, DEC-068). **Closes D-011** and re-enables the three reader-side tests. |
| `4e6135b` | **The validity token and `revalidate`** (§3.1, DEC-051/070). |
| `9efb9a7` | **Bucket table + packed entry codec** (DEC-102/103). |
| `ea364af` | The radix runners are now in `run_all_tests`, where they always should have been. |

Full suite at `ea364af`: Core 441×2, Kernel 173, LibAlloc 38×2, vmsmalloc 31×2,
RCU 61×2, **radix 106×2**. Kernel builds and boots clean.

**D-011 is closed and that is the headline.** The three disabled tests in
`ConcurrentTest.cpp` are on, so the tree finally has reader-side concurrency
coverage; before this every concurrency test it had was writer-vs-writer. The
closure was mutation-verified, not merely green — restoring the synchronous
direct-slot release is caught by `readers_never_observe_a_torn_state` as an ASan
use-after-poison on a reader thread.

**D-029 is a new WATCH item** and the most important thing to know before
touching the refcounts: DEC-069's release-plus-acquire-**fence** spelling is
correct in the C++ model and **invisible to ThreadSanitizer**, which does not
model `atomic_thread_fence`. It has been folded into an `ACQ_REL` RMW at both
release sites. If a `Mapping`-constructor-vs-`offsetFor` race report ever comes
back, **do not re-silence it** — the alternative explanation is a real
use-after-free the oracle's poison window happens to miss.

### Growth's binding rule, and why it is smaller than it first looks

The remaining items in the suggested order are **growth and creation** (§5.6),
then placement, the control block, teardown and the enumeration API.

An earlier draft of this note treated growth as blocked on a core/policy seam
decision, on the reading that a concurrent growth makes an in-flight operation's
`{root, level, base}` *wrong*. **That reading is mistaken and is worth correcting
here, because it is the natural one and it inflates the work by an order of
magnitude.**

A stale binding within a section is an ordinary RCU snapshot. If a writer decodes
`(R, level 3, base B)` and a grower then publishes `(P, level 2, base B')`:

- growth does not relabel the old root — it allocates a new parent above it — so
  `(R, 3, B)` stays a *true* description of the node it names;
- nothing is retired and no count moves (§5.6's transfer-in-place), so `R` stays
  reachable as `P`'s child;
- a writer holding claims inside `R` publishes into slots that are exactly where
  they were, and its range fit `R`'s span, which growth only widens.

What §7.3 actually guards is narrower, and it is two things:

1. **Atomicity of the triple.** The failure is pairing a *fresh* root pointer
   with *stale* `{base, level}` — decode `P` in a new section but keep level 3
   and base `B` from the old one, and the writer computes bit indices for a node
   that is not shaped that way, "on a node it holds a valid claim in". DEC-103's
   packing gives that atomicity for free **provided all three fields come out of
   one load**. The rule is decode-together, not never-be-stale.
2. **The uncounted root pointer must not cross a section close** — and this one
   has teeth *because* of growth. Before growth `R` is the cluster root, which
   §6.4's walk never reclaims. After growth `R` is an ordinary interior node with
   a parent slot and a state word, so it becomes reclaimable the moment it
   empties, and a pointer held across a close can be freed underneath.

So: **decode the bucket word once at the top of each attempt, inside that
attempt's section, all three fields from one `protectWord` load.**
`runToCompletion` already opens one section per attempt, so nothing about the
attempt machinery moves — which is the part §6.5 took seven rewrites to get right
and the part not to disturb.

Two consequences, both benign and worth recognising rather than debugging later:
a writer working from the pre-growth view treats `R` as the cluster root and so
declines to reclaim it, while one working from the post-growth view treats it as
interior and may. The two differ in conservatism, not correctness. And
`CoreTree`'s existing `if (node == rootRef) return false` reclamation guard stays
correct under either, because it compares against whatever root *that attempt*
decoded.

§5.6's accounting rules are the part to re-read first and not paraphrase: **no
count moves on the old root, nothing is retired, nothing reverts.** A CAS-
published root is constructed at count 1 (pre-assigned, not a `+1` on a published
object); the old root's inbound reference **transfers in place** to the new
parent's child slot; a losing grower or creator **shallow-discards** its private
node, pre-assigned count notwithstanding. If an implementation wants a decrement
anywhere on the growth path, it has diverged.

### Still not started

Growth/creation, placement + the DEC-091 assignment seam, the control block +
creation sequence, teardown, the lookup/enumeration API, and the **in-kernel
entropy source** (DEC-063). The entropy source is deliberately untouched: it is
a security-sensitive component whose algorithm, seeding policy, reseeding and
RDSEED-unavailable behaviour are all choices, and picking them unilaterally is
not the kind of judgement call this project wants made in the background.

---

## 1. Where things stand

### Complete and green

| Phase | Content |
|---|---|
| 0 | UAGP oracle harness (DEC-052), seedable random source, fault injection |
| 1 | Core tree single-threaded: geometry, codec, dispatch, descent, subdivision, reclamation |
| 2 prereqs | `tryReservePerDomainStaticBuffer`, `DomainManagementLockGuard`, runtime `Domain::init`/`deinit` + block freelist, `drainAllQuiescent`, `protectWord`, `PageAllocator::tryAllocateSmallPage` |
| 2 | Ordering table + spelling check, claim protocol, two-pass shape, reclamation, §6.5 detachment **and decomposition**, RCU integration, the four gate asserts |

**Phase 2's exit gate is met except one item**, carried forward by user decision —
see §3 below.

### Test targets

All under `tests/`, built with `cmake --build build --target <T>`. **Run cmake from
`tests/`, not the repo root** — the root `cmake-build-debug` is the kernel build.

| Target | What |
|---|---|
| `KernelRadixTestRunner` | ASan + leak. Carries the DEC-052 oracle (`__asan_poison_*` has no TSan equivalent) |
| `KernelRadixTestRunnerTSan` | The primary release gate on this machine (ARMv8) |
| `KernelRadixProgressAuditRunner` | TSan + `CROCOS_RADIX_PROGRESS_AUDIT` — §6.7's maximum-holder assert |
| `KernelTestRunner`, `KernelVmsmallocIntegrationTestRunner`, `LibAllocTestRunner`, `CoreTestRunner{,TSan}`, `KernelRcuIntegrationTestRunner{,TSan}` | The rest |
| `run_all_tests` | Everything. **Run it at the end of a phase** — see D-021 |

Current: radix 86/86 on all three runners; KernelTestRunner 173/173; vmsmalloc
24/24; LibAlloc 38/38; Core 441/441. Kernel builds and boots clean
(`cmake --build cmake-build-debug --target run`, exit 0).

### Code layout

`kernel/include/mem/radix/`: `Geometry.h`, `Node.h`, `SlotCodec.h`, `Dispatch.h`,
`Ordering.h`, `Claim.h`, `Mapping.h`, `ProgressAudit.h`, `CoreTree.h`.
Tests and mocks in `tests/kernel/radix/`.

The core type is
`CoreTree<GeometryDescriptor G, typename Codec, unsigned DetachBudget = kDetachBudget>`.
Public surface: `init(rootLevel, base, domain)`, `apply(lo, hi, Mapping*)`
(`nullptr` clears), `lookup(va)`, `destroyTree()`, `walk(fn)`, `nodeCount()`,
`stats()`. `apply` handles decomposition internally, so callers only ever see
`Ok` or `OutOfMemory`.

---

## 2. Prerequisites Phase 3 must land first

Both are listed in `radix-tree-phase-3.md` and **neither exists yet**:

- **`vmsmallocTry` / `tryMake`, kernel-side** (D-003, vmsmalloc DEC-048). The
  radix mock substrate has provided `tryMake` since Phase 0, and *the entire tree
  is written against the failable contract* — nothing in `kernel/include/mem/radix/`
  names `make`. The kernel side is not free: both panic sites bottom out in
  `VMSubstrate::allocPage()`, whose failures live in `reserveFreeVA` (arena
  exhaustion) and `PageAllocator::allocateSmallPage` (physical exhaustion), so it
  needs a `tryAllocPage` and a failable path through both. §10's inverted hazard is
  the reason it cannot be skipped: a site written against never-null `make<T>`
  re-imports the userspace-triggerable panic through the back door.
- **In-kernel entropy source** (DEC-063, named out-of-spec work). Probing and
  backoff jitter consume it. The test side keeps Phase 0's seedable source.

---

## 3. Obligations carried into Phase 3

### 3.1 D-011 — three disabled tests, and Phase 2's last gate item

**This is the most important thing in this note.** Direct-slot `Mapping` releases
are still **synchronous**: a record displaced from a directly written slot (the
overwrite and clear rows) is released inline in the commit walk. §7.1 names this
exactly — "a synchronous release on a published node is a use-after-free with no
grace period at all — and it is the NATURAL implementation". A reader inside the
section that observed the old slot word takes its counted reference after the
writer has taken the count to zero. It reproduces as vmsmalloc's "Double free: bit
already set in freeBitmap".

The fix is DEC-068's `DeferredRelease` records, already a Phase 3 work item.
**Do not shortcut it by allocating a record per release** — DEC-068 forbids
precisely that: "Allocating here would make `munmap` an ALLOCATING operation … the
one operation that RELIEVES memory pressure the one that fails under it."

Spencer decided (2026-08-08) that the asynchronicity belongs to Phase 3 and Phase 2
ships with the gap. The agreed rider: **it is a carried-forward gate item, not a
closed one.** When `DeferredRelease` lands, re-enable all three tests in
`tests/kernel/radix/ConcurrentTest.cpp`:

- `DISABLED_radix_concurrent_readers_never_observe_a_torn_state`
- `DISABLED_radix_concurrent_expansion_and_reclamation_are_invisible_to_a_reader`
  — this is Phase 2's `rcuTortureForcedStall` gate item
- `DISABLED_radix_concurrent_subtree_replacement_is_atomic`

**The consequence to keep in mind while working:** with those three disabled,
*every* concurrency test the tree has is writer-vs-writer. The tree has had **zero
reader-side concurrency validation**. Treat the reader path as unproven until they
are back on.

### 3.2 D-010 — a stranded partial slot block

`reserveFreshSlotBlockLocked` reserves a multi-page slot array one page at a time
for P2-I4's per-page NUMA placement. A mid-block failure cannot hand the earlier
pages back (reservations are kernel-lifetime by contract) or recycle them (the
freelist holds whole blocks). Bounded by construction — needs both >32 CPUs and
physical exhaustion mid-block — and logged rather than silent. The clean fix costs
the per-page NUMA refinement, a deliberate user-confirmed decision; do not reverse
it unilaterally.

### 3.3 D-004 — a spec repair still queued

§7.2's edge-subdivision row states a minimal-shape figure (`0`) as general; it is
`+(k−1)` for the survivor's leaf count in general, exactly as DEC-066 already
caveated the middle-punch row. **The implementation is correct**; only the spec
text needs the caveat. Cheap, and worth doing before it traps someone.

---

## 4. What Phase 2 learned that Phase 3 will need

These are the things that cost time. They are not restatements of the spec.

**Only a claimed slot is frozen.** This was D-013, and it presented as two
unrelated symptoms — §5.3's occupancy over-count assert and a `Mapping` refcount
underflow seen as a use-after-poison. The cause was that `redispatchAgrees`
re-ran the dispatch but never compared the resulting row against the claim set the
read pass recorded, and `commit` then re-read slot words and acted on whatever it
found. Anything in Phase 3 that reads a slot word and acts on it must ask whether
the attempt holds that slot.

**Classify, don't reason.** D-013 was found by putting a forcing-function assert at
the write ("every slot commit mutates must be one this attempt holds a bit for")
and then, when it fired, adding *per-row* asserts to identify which transition was
reachable. That gave exactly one row, 8/8, in minutes. Enumerating the cases by
reasoning had produced a plausible and wrong answer — the recorded D-013 hypothesis
was wrong, and so was mine.

**Mutation-test any test whose failure mode is passing vacuously.** The
decomposition suite (D-022) and the progress audit (D-025) were both mutation-
tested, and in both cases the mutation *changed the design*: skipping empty slots
was caught, the rejected DCA site predicate was caught, and the reversed-acquisition
mutant revealed that my progress assert was missing one of the lemma's two
hypotheses. Recorded mutations that must stay caught are listed in D-022 and D-025.

**Asserts that fire on legal executions are the cardinal sin here**, and §11 says
so repeatedly and specifically. Before writing any progress or ordering assert,
re-read §6.7 — it names three formulations that are false, and §11 names a fourth
("each re-dispatch site is strictly deeper"). Phantom-bit and terminal-mask
failures are legal; they get counters. Never assert a non-blocking classification
(DEC-057) — lock-freedom and obstruction-freedom are *refuted*, not unproven.

**Reuse the dispatch rather than restating it.** §6.5's decomposition enumerates
six per-slot rows; the implementation does not re-implement any of them, because
they *are* §6.3's rows applied to a sub-range. A second table would need permanent
agreement with the first, and §6.5's own warning is that every paraphrase during
review introduced a fatal. §7.4's teardown is the same shape — a unit-decomposed
walk — so the same instinct should apply.

**Implement §7.4's teardown sequence in its stated order.** The phase spec says
every reordering reviewed was a fatal. This is the §6.5 warning again, one section
over.

**Run `run_all_tests` at the end of the phase.** D-021: `KernelTestRunner` had not
compiled for several commits because a sibling test file pinned the size-class
schema and nobody rebuilt that target. Verifying a phase by running that phase's
runner does not notice a target it broke.

---

## 5. Known flakes — not your bug

- **Core/RCU TSan after a full parallel rebuild**: `run_all_tests` reports ~16
  timeouts (AtomicBitPool, TreiberStack, `rcuConcurrent*`). All pass when re-run
  serially. Documented; see `[[project_core_tsan_post_rebuild_flake]]`.
- **`rcuTortureDeadSlotDoesNotUnboundLimbo`** fails `residue <= kResidueBound`
  about **3 runs in 12** under TSan. Measured at the same rate with and without
  the radix work, so it is pre-existing and independent — it counts retired
  *objects*, not memory. Distinct from the timeout flake above and **not yet
  investigated**; a small standalone task if you want one.

---

## 6. Conventions worth not rediscovering

- No STL, no `std::` in kernel code; use the Core library. Unit tests are the
  exception and may use the host standard library.
- Unit test files must open with `#include "../test.h"` then
  `#include <harness/TestHarness.h>`.
- Every atomic in the radix tree is spelled with a **named ordering constant** from
  `Ordering.h`; `OrderingSpellingTest.cpp` statically enforces it, and the
  claim→slot-read acquire edge is invisible to every test you can run (§6.6/§10),
  so review is the verification.
- Debug asserts compile out in release (`kassert.h`), which is what makes the
  forcing-function style affordable. The project's stance is trust-callers-in-
  release, check-in-debug.
- Ranges are floor-unit granular: `lo` aligned, `hi` at a unit's last byte. A
  misaligned `hi` trips "subdivision ran past the resolution floor" — it cost time
  twice while writing the decomposition tests.
- `arch::` constant casing is irregular (`arch::smallPageSize`,
  `arch::CACHE_LINE_SIZE`, `arch::MAX_PROCESSOR_COUNT`). The specs sometimes cite
  names that do not exist (`kSmallPageSize`); translate when reading.

---

## 7. Suggested Phase 3 order

`radix-tree-phase-3.md` lists the work items; this is a build order that keeps
something testable at each step.

1. **`vmsmallocTry` / `tryMake` kernel-side** (§2 above) — everything else
   allocates, and the tree is already written against the failable contract.
2. **`DeferredRelease` pools** (§7.1, DEC-068) — do this early rather than last.
   It closes D-011, re-enables the three tests, and gives the tree its first
   reader-side concurrency coverage, which everything after it benefits from.
3. **`Mapping` records** (§5.4) — the validity token, protection, max-protection.
4. **Bucket table + bucket codec** (DEC-102/103), then **growth and creation**
   (§5.6). Hazard the spec flags: growth accounting is the newest text in the
   document — **no count moves on the old root, nothing is retired, nothing
   reverts.** If an implementation wants a decrement on the growth path, it has
   diverged; stop and re-read §5.6/DEC-103.
5. **Placement** (§5.6) and the **cluster-assignment policy seam** (DEC-091). The
   cheap proof the seam is real is a second toy policy in the harness. The per-CPU
   cell holds a **bucket index**, never a decoded pointer.
6. **Control block + creation sequence** (DEC-082/101), then **teardown** (§7.4,
   DEC-065/100) in its stated order.
7. **Lookup + enumeration API** (DEC-083).

Two more traps the phase spec calls out and that are easy to get wrong: the
replenish primitive is **`barrier`, never `synchronize`** (`synchronize` does not
seal the caller's open bag), and records return to pools **before the section
close** on abandonment.

---

## 8. Commit history for context

```
e3b82bd  Phase 2 (7/n): the four gate asserts
7db1091  Phase 2 (6/n): §6.5 decomposition
2c117d2  Phase 2 (5/n): D-013, and the claim check that was missing
439a8b8  Phase 2 (4/n): concurrency tests, and the six defects they found
0d8705d  Phase 2 (3/n): RCU integration
17ec3f2  Phase 2 (2/n): the claim protocol and the two-pass shape
d9a7862  Phase 2 (1/n): the ordering table and its static check
73c4691  rcu/vmsmalloc: the Phase 2 prerequisite contracts
87e4a2f  Phase 1: the core tree, single-threaded
a58f70f  Phase 0: the UAGP oracle harness
```

Nothing is pushed; the branch has not been merged to `master`.
