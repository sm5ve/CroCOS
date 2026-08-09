# radix-tree implementation — deviations and spec findings

Running log for the implementation branch (`radix-tree`), kept per
[[feedback_spec_deviations]].

**Starting a fresh context?** Read `docs/radix-tree-HANDOFF-impl.md` first — it
says where the implementation stands, which entries here are live obligations
(D-003, D-004, D-010, D-011) and which are closed.

D-001 through D-020 were recorded during the autonomous overnight session of
2026-08-08, under a document-and-proceed delegation for anything short of a
fundamental error: every judgement call made without sign-off is recorded with
its spec citation and reasoning. Entries from D-021 on are from ordinary
sessions and carry their own dates.

Categories:

- **FINDING** — the spec is wrong or internally inconsistent. Nothing was
  changed in the spec; the implementation follows the reading given.
- **CHOICE** — the spec is silent or underdetermined and the implementation had
  to pick. States what was picked and why.
- **GAP** — something the spec requires that is deliberately *not* implemented
  yet, with the phase it belongs to.

Entries that have since been resolved say so in their heading, and keep the
original text below the resolution — the reasoning is why the decision was
needed.

---

## D-001 — RESOLVED 2026-08-08 (user-approved) — vmsmalloc DEC-049's slots-per-slab figure is off by one at 192 B

**Resolution**: the layout change was taken. `slotCount` / `slot0Offset` now align
slot 0 to the class's **contractual** alignment rather than to
`max(slotSize, 16)`. Spencer's reasoning: "RadixVM is the only consumer of
vmsmalloc, it would be silly not to tune it for our purpose."

Realised: 192 B goes 19 → **20** (DEC-049's stated figure, reached by changing the
layout rather than by asserting it was already there), 320 B goes 11 → **12**.

**One consequence worth recording, because D-001 did not name it**: contractual
alignment is `<= max(slotSize, 16)` for every non-power-of-two class, so the rule
reaches **96 B** too, which goes 39 → **40**. Its *promise* is unchanged at 16
(DEC-025 keeps it there) — only its layout moved. Power-of-two classes are
untouched, since for them the two formulas coincide. Aligning to a smaller value
can only move slot 0 earlier, so no class can lose a slot.

Pinned by `SizeClassTest.cpp::vmsmalloc_dec049_realised_packing` and
`::vmsmalloc_d001_the_alignment_rule_reaches_every_non_pow2_class`, and by the
per-class `static_assert` block in `SlabLayoutTest.cpp`. Verified with the full
test suite, a kernel build, and a clean QEMU boot (exit 0).

The original finding follows, since the inconsistency it documents is the reason
the change was needed.

---

## D-001 (original finding) — vmsmalloc DEC-049's slots-per-slab figure is off by one at 192 B

**Spec**: `vmsmalloc.md` DEC-049 — "Packing: 20 slots per slab at 192 B, 11 at
320 B."

**Realised**: 19 at 192 B, 11 at 320 B (pinned by
`tests/kernel/vmsmalloc/SizeClassTest.cpp::vmsmalloc_dec049_realised_packing`).

**Why**: DEC-001's slot-0 formula aligns slot 0 to
`max(slotSize, alignof(max_align_t))`, i.e. to a *192-multiple* for the 192 B
class. Past the 32 B descriptor and the 192 B bookkeeper that lands slot 0 at
384, leaving `(4096 − 384) / 192 = 19`. The cited 20 is what a 64 B slot-0
alignment would give — but DEC-049's own argument for why the 64 B contract is
free is precisely that *no layout changes*: "DEC-001's slot-0 formula already
lands every slot of these classes on a 64 B boundary — the contract now matches
the layout". Both cannot hold at once. 320 B is unaffected (slot 0 at 320,
`(4096 − 320) / 320 = 11`), which is why only one of the two figures is wrong.

**Taken**: the normative sentence, i.e. *no layout change* — `slot0Offset` is
untouched and only `slotAlignment` was raised to report 64 for these two
classes. The slot count is a rationale figure with nothing depending on it; the
alignment contract is what `make<T>` enforces, and it holds either way.

**If 20 slots was actually wanted**, the change is to make `slot0Offset` use the
contractual alignment rather than `max(slotSize, 16)`, which would also move
320 B from 11 slots to 12. That is a real (small) memory win — ~5% more nodes
per slab at 192 B, ~9% at 320 B — but it is a layout change to a shipped
allocator affecting every class whose contractual alignment differs from
`max(slotSize, 16)`, so it is not something to take in-absentia. Flagged for a
decision.

---

## D-002 — CHOICE — `slotAlignment` reads a table rather than a derived rule

**Spec**: vmsmalloc DEC-001/DEC-022/DEC-025 as amended by DEC-049.

The pre-existing `slotAlignment(c)` derived the answer from `slotSize(c)`:
power-of-two classes get their own size, everything else 16. DEC-049 makes 192
and 320 promise 64, which no size-derived rule produces without also changing
what 96 promises (96's realised layout would support 32, but DEC-025 says
explicitly "the 96 B class remains the 16 B case").

**Taken**: an explicit `kSlabSlotAlignments` array 1:1 with `kSlabSizeClasses`,
with `validateAllClasses` extended to check each promised alignment is a power
of two, divides the slot stride, and divides `slot0Offset`. Promising only what
the spec promises; the static_assert is what stops a future schema retune from
silently invalidating a promise.

Rejected: keying the exception on the literal sizes 192/320 inside the derived
rule — that breaks the moment DEC-003's "tunable based on RadixVM measurements"
provision is exercised, which is the whole point of the schema being an array.

---

## D-003 — GAP — the real `vmsmallocTry` / `tryMake` is not implemented yet

**Spec**: vmsmalloc DEC-048; radix DEC-075; `radix-tree-phase-3.md` ("Outside
prerequisites").

The radix mock `<mem/VMSubstrate.h>` provides `tryMake<T>` from Phase 0, because
Phase 0's work items require it (the scriptable-null fault-injection hook) and
because the tree must be *written* against the failable contract from its first
line — §10's inverted hazard is that a site written against never-null `make<T>`
re-imports the userspace-triggerable panic through the back door. Nothing in the
tree names `make`.

The **kernel-side** `vmsmallocTry` / `tryMake` is not implemented. It is listed
as a Phase 3 prerequisite, not a Phase 2 one, and it is not free: both panic
sites bottom out in `VMSubstrate::allocPage()`, whose own failures are inside
`reserveFreeVA` (arena exhaustion) and `PageAllocator::allocateSmallPage`
(physical exhaustion). Making it failable means a `tryAllocPage` and a failable
path through both, which is a change to a shipped allocator with no consumer
until Phase 3.

Since Phases 0–2 run entirely in the userspace harness, nothing is blocked. The
work belongs with Phase 3, where its first real caller (placement, the record
pools, the root page) arrives.

**What is already done for it**: the class-dispatch switches in `vmsmalloc.cpp`
now carry a `static_assert(kNumSizeClasses == 10)` so they cannot silently miss
a class, which is the trap a failable-path edit would otherwise be layered on
top of.

---

## D-004 — FINDING — §7.2's edge-subdivision row states a minimal-shape figure as general

**Spec**: `radix-tree.md` §7.2, the naming-slot transition table:

> | subdivision, **edge** (`mmap` at a leaf's start or end) — child holds
> survivor + the *new* record | **0** for the old record |

**Realised**: 0 only when the survivor fits in ONE child slot. When the survivor
is wider — which is the common case — the delta is `+(k − 1)` where k is the
survivor's leaf count, exactly as for the middle punch.

**Worked case** (pinned by
`CoreTreeTest.cpp::radix_tree_edge_mmap_wide_survivor_takes_the_per_transition_delta`):
a C2-rooted tree, 64 KiB slots over sixteen 4 KiB C3 children. A full-span leaf
edge-punched by one page leaves a survivor spanning fifteen C3 slots, so the old
record's count goes 1 → 15, not 1 → 1.

**Why this is a finding rather than a misreading**: §7.2's *middle-punch* row was
given exactly this caveat by DEC-066 — "k = 2 only in the minimal shape; a wide
punch through a coarse leaf builds DEC-066's spine and k is its survivor-leaf
count". The edge row is the same dispatch row with the same wide-survivor
behaviour and did not get the same treatment. §6.3's action text is correct and
general ("A survivor wider than one child slot therefore becomes **many**
leaves"); it is the §7.2 table cell that reads as a rule.

**Taken**: the per-transition rule, which §7.2 itself calls the normative
statement ("Everything else falls out, which is the point of stating it this
way"). The implementation applies it literally — `+1` per new naming slot at
publish, `−1` for the slot that stopped — and takes no special case for either
subdivision shape.

**Risk if the table is read as a rule**: an implementation that hard-codes 0 for
the edge case under-counts by 14 in the worked case above and frees the record
while fifteen slots still name it — the premature-free DEC-048 exists to prevent,
reached through the row that was supposed to be the safe one.

**Suggested spec repair**: give the edge row the DEC-066 caveat, e.g. "**0** in
the minimal shape (survivor fits one child slot); in general +(k − 1) for the
survivor's leaf count k, per the transition rule".

---

## D-005 — CHOICE — the two codec instances differ in base policy, not in arithmetic

**Spec**: §5.2 / DEC-023 — "Each codec ... has two instances: an **uncompressed**
one for the userspace harness, whose mock arena is in the low half where the
kernel's pointer-compression assumptions are false, and a compressed one for the
kernel."

A *literally* uncompressed harness codec cannot exist: a 64-bit pointer plus a
24-bit sub-range does not fit in 64 bits. What is actually false under test is
the kernel's **base** (VMSubstrate at root[510]) and its canonical-bit pattern —
which DEC-023's own framing anticipates: "it makes the pointer-compression
assumptions **parameters** rather than baked-in facts".

**Taken**: one `SlotCodec<G, BasePolicy>` with two policy instances. The
arithmetic (field widths, in-place pointer storage, tag bits, the guard bit) is
shared; the policies differ in where the base comes from and what each asserts.

**Consequence, and why it is an improvement**: §10 lists "the harness is
structurally blind to the kernel codec's compression arithmetic" as a hazard.
Sharing the arithmetic narrows that blindness to the base and the canonical-bit
fact — the parts that genuinely differ — instead of leaving the shift/mask logic
untested until Phase 5.

The harness policy additionally binds the arena's **real extent** (64 MiB) rather
than the nominal 512 GiB window, so an address past the arena is rejected. Without
that, an out-of-arena pointer encodes and decodes cleanly while naming memory the
tree does not own, and the "assert your own base and range" requirement is a
formality. The kernel policy lands with Phase 5, where a kernel build first exists
to bind it.

---

## D-006 — CHOICE — empty-node reclamation is implemented in Phase 1

**Spec**: §6.4; `radix-tree-phase-2.md` lists reclamation as Phase 2 work, and
Phase 1's exit gate does not mention it.

Reclamation is mandatory (DEC-017) and its *serialized* form is decidable without
any of Phase 2's machinery. Leaving it out would have made every Phase 1 churn
test grow monotonically, which in turn makes the memory-returns-to-baseline
target untestable and the validator's "no empty non-root node survived" check
unwritable.

**Taken**: implemented, in the shape Phase 2 needs rather than the easy one — the
emptiness decision is computed from the node's occupancy count after the
operation's clears, bottom-up, which is the same computation DEC-064 specifies
against claim-frozen priors. Phase 2 replaces *where the counts come from*, not
the decision. The walk terminates at the cluster root, which is never a
candidate.

---

## D-007 — GAP — Phase 1 releases are synchronous

`§7.1`'s "every release is deferred" rule cannot be honoured before RCU
integration exists, so Phase 1's `releaseNamedMapping` and subtree release run
inline. This is the one place Phase 1 knowingly does the thing §10 flags as "the
natural implementation, since the commit walk already visits every node".

It is safe here only because Phase 1 is single-threaded with no readers. Phase 2
replaces it with unlink → `retire` → deleter, and the structure is already shaped
for that: `releaseSubtree` releases node by node, each node releasing only the
Mapping references *its own* leaf slots hold, child-node slots releasing nothing
— which is exactly the deleter's contract, running early.

---

## D-008 — CHOICE — `EpochDomain::checkQuiescent()` made public

**Spec**: RCU-DEC-043 — "`deinit()` ... requires quiescence (no open sections, no
undrained bags), **debug-asserted**".

The check already existed as `EpochDomain::checkQuiescent()`, but it was private,
reachable only through `DebugIntrospection` — which is gated behind
`CROCOS_RCU_TEST_HARNESS` and therefore **does not exist in a kernel build**.
`deinit` is the explicit destruction path for dynamic domains and needs exactly
the check `~EpochDomain` carries for static ones, in the build that ships.

**Taken**: made it public, with a comment saying why. It is const, debug-only and
idempotent, so exposing it grants no ability to mutate a domain. The alternative —
routing `deinit` through a test-only header — would have compiled, and would have
silently removed the check from every non-harness build.

---

## D-009 — FINDING (harness) — the RCU block freelist outlived the arena it points into

Not a spec defect: a defect in the *interaction* between RCU-DEC-043's freelist
and the userspace harness, found by two of the new lifecycle tests.

The freelist is process-global, which is correct in the kernel — VMSubstrate's
static-buffer window lives as long as the kernel does, so a recycled block is
valid forever. The harness mmaps a **fresh arena per test** and munmaps it
afterwards, so a block left on the freelist by test N is a dangling pointer into
unmapped memory by test N+1, and the next `Domain::init` hands it out as a slot
array.

It surfaced as two apparently unrelated failures, which is what makes it worth
recording:

- a test asserting a *fresh* reservation got a stale recycled block instead;
- a test scripting a reservation failure never saw it fire, because the freelist
  satisfied the request without ever calling the reservation.

**Fixed** by `rcu::test::resetDomainManagementState()`, called from the
lifecycle harness's teardown. Any future harness that calls `deinit` must call it
too; the requirement is stated in `tests/kernel/rcu/DebugIntrospection.h`.

---

## D-010 — GAP — a partially-reserved multi-page slot block is stranded

`reserveFreshSlotBlockLocked` reserves the slot array one page at a time, for the
per-page NUMA placement P2-I4 documents. With the failable reservation, a
mid-block failure cannot hand the earlier pages back — reservations are
kernel-lifetime by contract — and cannot recycle them either, since the freelist
holds whole blocks. Those pages are stranded in the static-buffer window.

Bounded by construction: reaching it needs **both** a machine with more than 32
CPUs (so a block spans pages) **and** physical exhaustion mid-block, at which
point the system is already failing allocations everywhere. It is logged rather
than left silent.

The clean fix is to reserve the whole block in one call, which costs the per-page
NUMA refinement — a deliberate, documented decision (P2-I4, user-confirmed
2026-08-01) that should not be reversed in-absentia.

---

# Phase 2 findings

The concurrency tests were where Phase 2 earned its keep: they found seven real
defects in three hours, five of which are now fixed. Every one was a rule the
spec states and the implementation had not honoured — which is the useful kind
of finding, because it means the spec was right and the code was behind it.

## D-011 — DEFERRED TO PHASE 3 by user decision 2026-08-08 — direct-slot `Mapping` releases are still synchronous

**Decision** (Spencer, 2026-08-08): the asynchronicity is a feature that belongs
in Phase 3; `DeferredRelease` is already a listed Phase 3 work item and the phase
boundary stays where the plan puts it. Phase 2 exits with this gap known.

**Rider, agreed at the same time**: this is a **carried-forward** exit-gate item,
not a closed one. Phase 2's gate names `rcuTortureForcedStall` pinning a reader
across expand-then-reclaim, and that test cannot run until DEC-068 lands — so
**Phase 3's gate must include re-enabling all three D-011 tests**, or the
checklist quietly forgets them. The coverage this defers is specific and worth
naming: with these three disabled, *every* concurrency test Phase 2 runs is
writer-vs-writer. Phase 2 ships with no reader-side concurrency validation at all.

The finding as originally recorded follows.

---

## D-011 (finding) — direct-slot `Mapping` releases are still synchronous

**Spec**: §7.1 — "Two reclamation authorities compose only because **every
release is deferred**. A synchronous release on a published node is a
use-after-free with no grace period at all — and it is the NATURAL
implementation, because the commit walk already visits every node, so releasing
there costs nothing and looks like tidiness."

That is verbatim what the implementation does for a `Mapping` displaced from a
**directly written slot** (the overwrite and clear rows): `releaseNamedMapping`
runs inline in the commit phase.

**Consequence**: a reader inside the section that observed the old slot word
takes its counted reference *after* the writer has taken the count to zero and
destroyed the record — resurrection, then a double free. Reproduced as
vmsmalloc's "Double free: bit already set in freeBitmap".

**Not fixed, deliberately.** The fix is DEC-068's `DeferredRelease` records,
which the phase plan schedules for **Phase 3** — and that scheduling is itself
the finding, because a concurrent reader taking a counted reference is Phase 2
functionality. The obvious shortcut (allocate a record per release) is
explicitly forbidden by DEC-068: "Allocating here would make `munmap` an
ALLOCATING operation … the one operation that RELIEVES memory pressure the one
that fails under it." Taking that in-absentia is not a call I should make.

The **detach path is already correct** — those releases ride node deleters and
are deferred by construction. The gap is precisely the directly-written-slot
rows.

Disabled by this: `DISABLED_radix_concurrent_readers_never_observe_a_torn_state`,
`DISABLED_radix_concurrent_expansion_and_reclamation_are_invisible_to_a_reader`,
and — established 2026-08-08, see D-014 — 
`DISABLED_radix_concurrent_subtree_replacement_is_atomic`.

## D-012 — FIXED — `lookup` returned a raw pointer, not a counted reference

§3.1 requires a lookup to return a **counted reference**, acquired inside the
observing section (§7.3's acquisition law). The first implementation returned a
raw `Mapping*`, so the very first concurrent reader test dereferenced a record
whose grace period had ended. `LookupResult` is now move-only and releases on
destruction.

Caught by the Phase 0 oracle as a use-after-poison; silent without it.

## D-013 — FIXED 2026-08-08 — commit acted on slots it held no claim on

`radix_concurrent_contended_writers_all_complete` failed 100% of runs with four
CPUs on a single node. Now passes 10/10 on ASan and under TSan, and is enabled.

**The recorded hypothesis was wrong, and usefully so.** It read the symptom
("occupancy count exceeded the valence", §5.3's assert) through §6.6's
double-increment shape: a writer re-running its dispatch on a pre-claim value.
The actual mechanism is simpler and sits one layer down. The first run of this
session did not even reproduce that assert — it reproduced a
*use-after-poison on `Mapping::releaseRef`*, an over-**release**. Same cause,
two faces, and chasing either symptom alone would have missed it.

**Root cause.** Only a slot the attempt holds a claim bit on is frozen.
`redispatchAgrees` re-runs the dispatch at every site, but never compares the
resulting row against the set the read pass recorded — so §6.1's rule, *"a row
that changed means the set the pass computed is wrong; the answer is to discard
and retry, never to extend the set in place"*, was implemented as the re-run
with the comparison missing. `commit` then re-reads each slot word and acts on
whatever it finds.

The reachable shape, confirmed 8/8 by classifying the transition rather than
reasoning about it, is exactly one row: **`writes=0`, `ClearSlot`, unheld**. A
munmap's read pass sees a slot empty — clearing an empty slot is a no-op that
takes no claim bit — and a concurrent writer fills it afterwards. Commit dispatches
`ClearSlot` on a slot nobody reserved, which:

- moves an occupancy count without the interlock → §5.3's assert, *the only
  detector over-counting has*; and
- releases a `Mapping` reference the attempt never took → refcount underflow,
  surfacing as the use-after-poison.

**Fix, in two parts.** `redispatchAgrees` now rejects any row that writes a slot
the claim set does not cover, which is §6.1's stated check finally written down;
this is what prevents a **lost write** on the `writes=1` side, by forcing a retry
that re-plans and claims the slot. And `commit` declines a writing row it holds no
bit for, because the window between re-dispatch and commit is real and only
claimed slots stay validated. Declining is not a patch over a race — it is the
correct answer: the interloping write landed after this operation's claims, so
the linearization in which the clear happened first and the write second is legal,
and it is the one where the new mapping survives.

A `writes=1` row reaching commit unclaimed would be a lost write rather than a
legal ordering, so it stays a debug assert. It should be unreachable — a writing
operation takes a bit for every slot it touches, including empty ones — and the
assert is what keeps that reasoning honest if a future dispatch row breaks it.

Instrumented as `TreeStats::unheldRowsSkipped`. Under the four-CPU contended run:
1200 completions, 48 skips (ASan) / 29 (TSan) — rare, and a number that climbs
into the operation count would mean the read pass is under-claiming rather than
losing a genuine race.

**Method note.** The fix came from a forcing-function assert placed at the write
(*"every slot commit mutates must be one this attempt holds a bit for"*) rather
than from reading the commit path. That assert is kept.

## D-014 — NOT A SEPARATE DEFECT — it is D-011

Recorded as "intermittent; shares the contended path with D-013". It does not.
With D-013 fixed, `radix_concurrent_subtree_replacement_is_atomic` fails
immediately and deterministically as *"Double free: bit already set in
freeBitmap"*, with a use-after-poison **read on a reader thread** — D-011's
signature exactly.

The mechanism: only the **first** round replaces a populated subtree, via the
detach path, which is correctly deferred. Every round after it finds slot 0
already holding the coarse leaf and dispatches to `OverwriteLeaf` — a *directly
written* slot, whose displaced `Mapping` is released synchronously in the commit
walk while readers hold counted references to it. It read as "intermittent"
only because it depends on reader timing.

Re-disabled under D-011, and re-enables with the other two when DEC-068's
`DeferredRelease` records land in Phase 3. D-014 is retired as an entry.

## D-015 — FIXED — reclaiming a node without holding the interlock

`commit` reclaimed any child it computed as empty, including one the read pass
had not nominated as a **candidate** — because candidacy comes from a RELAXED
advisory occupancy load that can be stale-high.

Marking a node this attempt does not hold violates invariant 19 ("every mark is
taken under a whole-node claim, on all three paths, no exemption"), and because
the mark is irreversible and blocks every future claim, it leaves the node
permanently marked AND STILL LINKED. §6.8 describes the consequence exactly:
"every inserter retries from the parent, and the range becomes silently
unmappable for the address space's life." Observed as a hard stall.

§6.4's other half is now implemented: "A node that empties but was NOT a
candidate is simply not reclaimed on this pass."

## D-016 — FIXED — missing freeze-and-verify re-walk

§6.5's phase one ends with a re-walk under the claims requiring the discovered
subtree to equal the recorded one, because a concurrent placement can legally
build nodes into a hole between the unclaimed read pass and the claims landing.
It was not implemented; detachment therefore marked nodes it did not hold.

## D-017 — FIXED — backoff was deterministic, not randomized

§6.7 property 3 excludes livelock **probabilistically**, and DEC-097 makes
randomized backoff the sole mitigation. The implementation used a deterministic
`1 << attempt` spin, which leaves threads that started together still aligned
after every doubling — exactly the timing-aligned mutual abort the randomization
exists to break.

Measured: four CPUs on one node stalled indefinitely (~100 of 1200 operations);
two CPUs completed fine, which is the signature of alignment rather than
deadlock. With jitter mixed from the CPU id, four CPUs complete.

## D-018 — FIXED — surplus allocations leaked on the SUCCESS path

§9: "Surplus allocations are shallow-discarded." The implementation discarded
them on abort but not on success — and a prebuilt subdivision subtree genuinely
can go unused on a successful attempt, because the read pass runs unclaimed and
the row can change before the claim lands. Leaked one 288 B node per occurrence,
only under concurrency.

## D-019 — FIXED — a prebuilt subtree was not validated against current content

Re-dispatch checked that a `Subdivide` row still had *a* pending subtree, not
that the subtree was built for the slot's *current* content. A row that stays
Subdivide while the leaf comes to name a different record would publish the old
record's survivors. The pending now records the word it was built from.

(Comparing raw slot words is correct **here and only here** — same slot, same
level. §3.1's validity token cannot do it, because it compares across levels
where a level-relative range field makes a bit-identical word mean a halved
range.)

## D-020 — FIXED — per-tree mutable state raced

`retiredThisOperation` was a plain `bool` member of the shared tree, written by
every CPU. TSan caught it; it now lives on the attempt.

---

# Session findings — 2026-08-08 (daytime)

## D-021 — FIXED — `KernelTestRunner` had not compiled since the DEC-049 commit

Not a spec defect: a stale test file, recorded because of how it was missed.

`tests/kernel/vmsmalloc/SlabLayoutTest.cpp` pins the size-class schema entry by
entry — `kNumSizeClasses == 8`, `kSlabSizeClasses[6] == 256`, and the per-class
`slotCount` table. DEC-049 (`218f60f`) inserted the 192 B and 320 B classes,
which shifted every index above 96 and made the schema 10 classes. The file was
not updated, so **`KernelTestRunner` failed to compile from that commit onward**,
with seven static-assert failures. Confirmed by stashing this session's changes
and rebuilding the target at the previous tip.

**Why it went unnoticed**: the branch's work was verified by running the *radix*
runners, which build and pass independently. Both were green — and reporting
"both gates green" on that basis was true of what was run and misleading about
the tree. `SlabLayoutTest.cpp` lives in `KernelTestRunner`, a target nothing in
the radix loop builds.

**Fixed**: re-pinned against the 10-class schema, including the two new classes
and the D-001 slot counts. Two *runtime* tests in the same file were stale the
same way and only surfaced once it compiled again —
`VMSubstrateSlab_DescriptorAccessors_LookupBySizeClass` (class 7 is 256 now, not
512) and `VMSubstrateSlab_SizeClassFor_BoundaryWalk` (the boundary walk skipped
192 and 320 entirely). `KernelTestRunner` is 173/173.

**Worth keeping**: a per-phase verification loop that only runs the phase's own
runner will not notice a sibling target it broke. The whole-suite target is the
check, and it belongs at the end of a phase rather than only at the end of a
session. Note also that a full parallel rebuild makes `run_all_tests` report ~16
TSan timeouts in Core/RCU that all pass when re-run serially — the documented
post-rebuild flake, not a regression.

## D-022 — §6.5 decomposition implemented; the per-slot rows are the ORDINARY dispatch

Phase 2's largest remaining work item. Over-budget detachment previously returned
`NeedsDecomposition` to the caller; it now decomposes.

**Shape taken.** `apply` splits into `runToCompletion` (one unit: the §6.1
attempt/retry loop) and `decompose`. The site is found by descending while the
whole range still fits one slot and that slot holds a child — §6.5's "deepest
node containing the operation's *entire range*", anchored to the RANGE, which is
what makes both rejected predicates unreachable. Every intersected slot at the
site, ascending VA, is then handed to an ordinary attempt over its clipped
sub-range, recursing when that reports `NeedsDecomposition`.

**The one judgement call worth recording**: §6.5 enumerates six per-slot rows
(covered leaf, fully- and partially-intersected empty, partially-covered leaf,
partially-covered child, fully-covered child). These are **not re-implemented**.
Handing each clipped sub-range to the ordinary attempt path makes the ordinary
dispatch produce exactly those six rows, because they *are* §6.3's rows applied
to a sub-range. Writing them out again would create a second table that has to be
kept in agreement with §6.3 forever — and §6.5's own warning is that every
paraphrase during review introduced a fatal. Reuse makes "a decomposed `MAP_FIXED`
over a range with holes maps exactly what the unit path would — no more, no less"
true by construction rather than by inspection.

Deliberately **not** asserted, per §6.5's explicit warning: "one detachment site
per unit" and "claims ≤ one subtree" fire on legal executions. The structural
check is the claim set's fixed capacity.

**Termination** is the property the shape has to earn, since the recursion is on
the same entry point. Per-slot recursion strictly descends (the site is the
deepest node containing the range, so a sub-range either straddles several slots
— each strictly smaller — or lands in a slot with no child, which has no subtree
to blow the budget). Two asserts hold that: the sub-range must be strictly
smaller, and a depth ceiling. The depth assert is not decorative — it was one of
the two things that caught mutation B below.

**`detachBudget` is now a template parameter** of `CoreTree`/`ClaimSet`,
defaulting to `kDetachBudget`. §11 specifies "a tiny geometry with a tiny
`detachBudget`", and decomposition is otherwise unreachable on any tree small
enough to assert over. DEC-077 calls 64 provisional, so this is a knob the design
already expects to turn. At the tiny geometry the budget has to be **1**: a
level-2 subtree is 5 nodes but a level-3 one is 1, so at 2 all three §11 cases
fit in a single unit and would have passed while testing nothing.

**Tests** (`tests/kernel/radix/DecompositionTest.cpp`) are §11's three cases plus
a fourth. Each runs the identical sequence on two trees differing ONLY in budget
and requires agreement at every floor unit — §11 words every case as "equals the
UNIT PATH's", which is a relative property, so the second tree is the oracle. The
fourth case is the polarity check: a `MAP_FIXED` over the same holes must FILL
them, which stops "skip every empty slot" from being an acceptable fix for the
munmap case.

**The suite was mutation-tested**, because a decomposition test that passes
vacuously is the failure mode here:

- *skip empty slots in the decomposition loop* (the natural "optimization") →
  caught by the map-over-holes case.
- *anchor the site to where the range starts rather than to the whole range* (the
  shape of the rejected DCA predicate) → caught by all four, via the
  orphaned-tail assertion and the depth assert.

**Unrelated flake observed while verifying**: `rcuTortureDeadSlotDoesNotUnbound
Limbo` fails `residue <= kResidueBound` about 3 times in 12 under TSan. Measured
at 3/12 both with these changes and at the pre-session baseline, so it is
pre-existing and independent — it counts retired OBJECTS, not memory, so neither
D-001's slab packing nor the radix work reaches it. Distinct from the documented
post-rebuild timeout flake; worth its own investigation.

## D-023 — FINDING — §11's "mark is last" exemption list implies a formulation that is false

**Spec**: §11 — "The mark is the last acquisition on its path | Debug assert,
**exempting the post-mark parent-slot store** in subtree detachment and the
post-mark bucket CAS in teardown's final per-cluster unit (DEC-100)."

The exemption list only makes sense against an assert that guards **writes**.
Against one that guards **acquisitions** — which is what the row's own title says
— neither exemption is needed, and §6.5 says so outright: the parent-slot store
"is not an acquisition after a mark, and a naive 'mark is last' assert must
exempt it explicitly."

**The write-based formulation is not merely inconvenient, it is false.** `commit`
iterates the site node's slots in one pass, so an ordinary legal execution —
slot 3 dispatches to `DetachChild` (which marks), slot 4 to a plain `WriteLeaf` —
publishes after a mark with no exemption available. Any implementation that
guards writes fires on that, which is precisely the class of assert §11 spends
four sentences warning against elsewhere ("fires on a legal execution").

**Taken**: the assert guards acquisitions (`acquireAll`), which is the row's
stated property and needs no exemptions.

The exemptions are still represented in the code, because the *reason* they were
listed is real and Phase 3 adds the second one. Detachment's parent-slot store
goes through a distinct `publishAfterMark` entry point, and reclamation's unlink
through `publishUnderChildClaim`, so both carve-outs are visible at their sites
rather than living only in a comment — and teardown's bucket CAS has an obvious
home when it arrives.

**Suggested spec repair**: reword the row's method to "Debug assert over
acquisitions; note that a naive write-based form must exempt the post-mark
parent-slot store and teardown's bucket CAS — and is false regardless, since a
commit that detaches at one slot and writes at a later one publishes after a mark
on a legal execution."

---

## D-024 — DEFECT (found by the gate assert, fixed) — claims were released after the section closed

Not a spec defect: the spec is unambiguous and the implementation was behind it.
Found by implementing §11's "no claim is held across an allocation or a **section
close**", which is exactly the assert that was missing.

**Spec**: §3.2 / §7.3 — one attempt is one read section, and "the claim set is a
set of link-loaded node pointers and those do not survive a close".

**Was**: both `Retry` paths in `runAttempt` — a failed `acquireAll` and a
disagreeing re-dispatch — returned with claims still held. The release happened
in `abandon`, which `apply` calls **after** the `ReadGuard` scope has ended. So
every contended retry issued a `fetch_and` on nodes whose pointers were loaded in
a section that had already closed.

**Why it had not bitten**: a held claim bit blocks the whole-node `fetch_or` that
reclamation, detachment and teardown all require, so a node this attempt holds a
bit on cannot be marked and therefore cannot be retired or destroyed while the
bit is set. The window is real but the memory stays alive through it — which is
why it survived a TSan-clean concurrency suite and is the kind of latent
violation that only becomes a use-after-free when some later change (Phase 3's
teardown, most plausibly, which does not take claims the same way) breaks the
coincidence.

**Fixed**: both paths release inside the section. `abandon` now only
shallow-discards unpublished allocations, which is safe outside a section because
those are reachable by nothing, and it asserts that nothing is still held.

---

## D-025 — the four Phase 2 gate asserts, and what the progress one needed

§11's four remaining Phase 2 gate targets. One was already implemented; the notes
here are the parts that were not mechanical.

- **Marked-but-linked** — already present in `TreeValidator`, over the quiesced
  tree. No work.
- **Commit-boundary** — a per-attempt phase flag, asserted at every mark, publish
  and retire. Every commit-phase store now routes through `publish` (or one of
  the two named exemption entry points), so §11's "at EVERY slot write" is
  structural rather than a count of copies that has to stay correct.
- **Mark-last** — see D-023.
- **Progress** — the substantial one.

**The progress assert needed a registry, and the reason is worth recording.** The
assert must classify a failed acquisition into §6.7's three buckets, and the
state word cannot do it: a **transient phantom** and a **writer-held** bit are
bit-identical in the word, differing only in whether some writer currently
considers itself the owner. Only the writers know, so each writer publishes what
it holds and a failing writer asks. (The **terminal mask** is the easy one — it
is visible in the word.)

A naive registry read races, and concluding the wrong way fires the assert on a
legal execution. The audit uses a seqlock-shaped window instead: each writer
brackets its published set with an odd/even epoch, and a failing writer samples
every peer's epoch **before** its `fetch_or` and again after. An epoch that is
unchanged and even across that window proves the peer's held set did not move
while we failed, so reading it is exact. Any epoch that moved makes the event
*inconclusive* and it is counted rather than asserted — deliberately biased to
silence.

It compiles to nothing unless `CROCOS_RADIX_PROGRESS_AUDIT` is defined, since
publishing costs two epoch bumps and two stores per claim on the hottest path in
the protocol. New target `KernelRadixProgressAuditRunner` (TSan) turns it on. A
test reports the classification split and requires it to be non-zero, so the
audit cannot pass by never running: a representative four-CPU run sees ~274
writer-held, ~198 phantom, ~19 terminal-mask and ~81 inconclusive failures.

**One formulation correction, found by mutation-testing.** The first version
asserted only "we are the maximum holder and we failed against a writer-held
bit", dropping the lemma's other hypothesis — §6.7's proof reads "a failure **at a
greater site** would require a live holder at a greater site". Ascending
acquisition order is what makes that hold on every acquisition, which is why §6.7
calls the order "load-bearing for deadlock-freedom, not merely for the mark's
irreversibility". Both halves are now explicit.

That correction came from a reversed-acquisition-order mutant which the progress
assert caught only ~1 run in 11 — because an order violation voids the lemma's
hypothesis rather than contradicting its conclusion, so it is the wrong assert to
catch it with. **§6.8's order now has its own direct assert** in `acquireAll`
(each acquisition's site must exceed every site already held), which catches that
mutant 3/3 and deterministically. Note it is the order over *held* claims, not
"each re-dispatch site is strictly deeper" — §11 records that as an assert that
fires on legal executions.

---

## D-026 — CHOICE (2026-08-08, Phase 3) — the panicking `allocPage` path now really panics

Not a spec finding; a behaviour change in `kernel/mm/VMSubstrate.cpp` forced by
building DEC-048's failable sibling next to it, worth recording because it makes
an existing path *louder* rather than leaving it alone.

**Was**: `reserveFreeVA`'s arena-exhaustion handling was
`assert(T != SIZE_MAX, "VMSubstrate arena exhausted")` — a debug-only check
(`kassert.h` compiles out in release). In a release build an exhausted arena
therefore walked on with `T == SIZE_MAX` and computed a wild VA. Underneath it,
`PageAllocator::allocateSmallPage` discards `allocatePages`' short count and
returns a default-constructed `phys_addr`, so physical exhaustion mapped
**physical page zero** and returned successfully. vmsmalloc DEC-012 says
"arena exhaustion panics"; neither half of that was true in release.

**Now**: the reservation helpers are failable end to end (they have to be —
`tryAllocPage` may not panic), and the two entry points sit on top of the same
body. `tryAllocPage` returns null; `allocPage` calls `PANIC` on the same
condition, in every build configuration. That is the behaviour DEC-012 already
specified, so no caller's contract changed — the callers that were written
against "never returns null" still are, they just now stop loudly instead of
silently mapping frame zero.

The one thing this does *not* fix is `PageAllocator::allocateSmallPage`'s own
silent short-count return, which is reachable from every other caller in the
kernel. `tryAllocateSmallPage` (the `arch::ProcessorID` overload was added here)
is the failable form; converting the rest is out of scope for this phase and is
not a radix obligation.

---

## D-027 — CHOICE (2026-08-08, Phase 3) — lazy page-table install had to become acquire-then-commit

The DEC-048 work item reads as if it were confined to vmsmalloc. It is not: both
panic sites bottom out in `VMSubstrate::allocPage()`, and that call is not a
single allocation. Reserving a fresh arena VA can lazily install up to two
hardware subtables, `D = ceil(processorCount / 64)` per-CPU dirty-bitmap pages
for a fresh leaf page table, and up to four radix occupancy-bitmap pages — every
one of them a separate `PageAllocator` call that can now fail.

**The hazard** is not the failure, it is the *partial* success. Levels of the
page-table chain are individually harmless to leave behind (an installed but
unused subtable is a valid empty table that the next call reuses, and the VA it
covers is still free), so those need no unwind. A **leaf PT carrying only some
of its dirty-bitmap pages** is not harmless — the dirty bitmap is how PTE
changes propagate to other CPUs without IPIs, and a hole in it is a silent
missed invalidation, which is exactly the class of defect that shows up much
later as a stale-TLB use-after-free (cf. [[project_vmsmalloc_stale_tlb_bug]]).

**Taken**: `ensureSubtableInstalled` was split into an acquire phase and a
commit phase. Everything that can fail happens before the first store — the
child table page, all `D` dirty pages into a stack array bounded by
`arch::MAX_PROCESSOR_COUNT / 64`, and the pre-backing of leaf `T_first`'s radix
bitmap pages — and a failure at any of them frees what it took and returns
false with nothing written. After the parent entry is stored, nothing can fail:
`reserveLeafBit`'s bitmap pages are already mapped and are never unmapped, which
is why its internal backing call became a `PANIC` (a failure there would mean
the caller's pre-backing invariant is broken, not that memory ran out).

The VA reservation and the data page are taken in that order, not the reverse,
because a reservation is a claimed radix bit with no mapping behind it and
`releaseLeafBitFor` — factored out of `releaseLeafMapping` for this — gives it
back without any of the PTE teardown `freePage` would attempt on a VA that was
never mapped.

**Coverage note.** The vmsmalloc half of DEC-048 is tested
(`tests/kernel/vmsmalloc/FailableAllocTest.cpp`, both enumerated panic sites,
each followed by continued use of the allocator — the decision's justification
is "the null return composes", and a test that only checks the null return
verifies none of it). The VMSubstrate half is **not** covered by a userspace
test and cannot be: the harness replaces `VMSubstrate` with an mmap-backed mock,
so there are no page tables to install. It is in-kernel-only, and Phase 5's
stress work is where it can be driven.

**Fidelity improvement worth noting**: the radix harness compiles the real
`kernel/mm/vmsmalloc.cpp`, so its mock `tryMake<T>` now calls the production
`vmsmallocTry` rather than the mock's own `vmsmalloc`. Every allocation the tree
makes under test now goes through the production failable allocator.

---

## D-028 — DEC-068's `DeferredRelease` records landed; **D-011 is CLOSED**

Phase 3's second work item, and the one the phase plan told us to do early rather
than last. It closes D-011, re-enables the three disabled tests in
`tests/kernel/radix/ConcurrentTest.cpp`, and with them gives the tree its **first
reader-side concurrency coverage** — every concurrency test it had until now was
writer-vs-writer.

**Verified by mutation, not by a green run.** Restoring the synchronous release
at the `OverwriteLeaf` row (and removing its draw, so the row is exactly the
pre-Phase-3 shape rather than a double release) is caught by
`radix_concurrent_readers_never_observe_a_torn_state` as an ASan use-after-poison
inside `Mapping::offsetFor` — on a READER thread. So the re-enabled tests detect
the defect they were disabled for, which a green run alone does not establish.

Four decisions worth recording, none of them restatements of §7.1.

**The draw happens in RE-DISPATCH, and the site is forced.** It cannot be in
commit, because a draw can fail and §6.1 says nothing after the commit boundary
may. It should not be in the read pass, which runs unclaimed and would over-draw
on every row a concurrent writer then changes. Re-dispatch is the one pass that
walks the *frozen* rows before the boundary. A consequence worth stating because
it looks like an omission: the accounting is **complete** at re-dispatch and
commit does no counting of its own. The two row sets are equal, not merely
nested — commit acts only on rows the attempt holds a bit for, and every row it
can skip (D-013's window) was a `NoOp` or a descend at re-dispatch, neither of
which draws.

**The held set is an intrusive list through the record's own `next` field**, not
an array. A record has three non-overlapping lives — in a pool, held by an
attempt, in flight through a retire (linked via `head` instead) — so one field
serves the first two. The alternative is a `deferredReleaseBound(G)`-sized
pointer array in every `Attempt`: ~1.8 KiB of stack per operation, on top of the
claim set, for a worst case almost nothing reaches.

**The replenish measures against the full population, not against emptiness.**
§7.1 says "if still short, it calls `barrier`", and `empty()` cannot express
that: a pool holding three records when the operation needs five is not empty and
is still short. The pool therefore carries a depth counter (RELAXED — it drives
only the heuristic, and `barrier` is unconditionally safe to call). With that,
a shortfall surviving a barrier is a **sizing error and not contention**, which
is now a forcing-function assert: `barrier` returns only once every record this
CPU retired is home, and the population is one operation's ceiling.

**The `≈230` ceiling is derived, and it is NOT the site bound.** §6.1's site
bound is 11 and is already in the code, which makes it the number an implementer
reaches for; the record ceiling is the **edge sum** — `valence(level 1)` plus
`2*(valence−1)` per level below — and is 230 under the kernel geometry. Both the
`static_assert` in `DeferredRelease.h` and a test recompute it from the
descriptor rather than transcribing it, per DEC-093.

**A harness change came with it, and it is not cosmetic.** The pool population is
allocated by the fixture and freed by it, so it is live for the whole of every
test — but a test asserting "this churn returned to zero live objects" means zero
objects *the tree* created. The oracle grew a fixture baseline that every count
is read relative to (`setAccountingBaseline`). It **subtracts at read time rather
than zeroing the counters**, which is the difference between honest and
convenient: zeroing would leave the fixture's own teardown destroying 230 objects
the counters believe were never constructed, and `noteDestroyed` reports that —
correctly — as a double destroy.

**And a testing lesson that cost time twice.** Several existing tests began
failing §7.2's naming-slot census with "structural count 2 but named by 1 leaf
slot". Not a defect: a displaced reference now comes home at grace-period end, so
between an operation and its grace period the counts *legitimately* exceed the
census. `RadixHarness.h` already said so in a comment nobody had needed yet.
Every count-checking validation now quiesces first.

---

## D-029 — WATCH ITEM — the refcount's acquire fence is invisible to the release gate

Found by the newly re-enabled reader-side tests, and worth recording carefully
because the remedy is a **spelling change against DEC-069** and the evidence for
it is statistical rather than a clean reproduction.

**The report.** `KernelRadixProgressAuditRunner` (TSan) intermittently reports a
data race between `Mapping::Mapping` — a *constructor*, on one thread — and
`Mapping::offsetFor` — a read, on another — at the same address, inside
`radix_concurrent_subtree_replacement_is_atomic`. Same address plus
constructor-after-read means the slab slot was **recycled**: a reader's last read
of a record, then that record's destruction, then a new record built in its slot.

**It is not a lifetime bug.** The reader holds a counted reference across the
read (`LookupResult` acquires inside the descent's section, §7.3), so the record
cannot be destroyed under it — and the ASan/oracle runner, which poisons on
destroy and would report a genuine use-after-free here, never does. What TSan is
reporting is a missing **happens-before edge**, not a missing guarantee.

**Where the edge lives, and why the gate cannot see it.** §6.6/DEC-069 spells the
structural decrement as RELEASE with an acquire **fence** on the zero-observing
one. That is correct in the C++ model: the destroying RMW reads a value in the
release sequence headed by the last holder's decrement, and an acquire fence
sequenced after it completes the edge — the classic `shared_ptr` idiom. But
**ThreadSanitizer does not model `atomic_thread_fence`**; it is a long-standing
limitation, and libstdc++'s `shared_ptr` carries explicit annotations for exactly
this. So on this project's default release gate the recycled-record edge simply
does not exist.

**Taken**: fold the acquire into the RMW — `fetch_sub(n, ACQ_REL)` — at both
refcount release sites (`Mapping::releaseRefs` and `deleteRadixNode`'s node
self-release), and delete the fence. New named constant
`kRefcountReleaseAcquire`; the old `kRefcountZeroFence` is removed rather than
left unused, since the §11 spelling check cannot tell a dead ordering constant
from a live one. This is never weaker than the spec's form — an acquire RMW at
zero is exactly what the fence was there to provide — and it costs one acquire on
a refcount decrement, which is not on the descent.

**Why this is a WATCH item and not a closed one.** The base rate is low: **2
reports in 13 full audit-runner runs** (~15%), and the warning never reproduced
in 8 full plain-TSan runs or in 10 isolated runs of the test alone. Sixteen
post-change runs were clean, which is suggestive (p ≈ 0.07 of that outcome if the
rate were unchanged) but is not proof. The honest statement is: the most likely
cause has been removed and the strengthening is correct on its own merits, but a
single further report would mean the diagnosis was wrong and there is a genuine
missing edge to find. **If one appears, do not re-silence it — the alternative
explanation is a real use-after-free that the oracle's poison window happens to
miss.**

**Spec follow-up owed**: DEC-069's "refcount release/acquire-at-zero" should
record the fence form as correct-but-untestable and the ACQ_REL form as the
implementation's, so the next reader of §6.6 does not "fix" it back.

---

## D-030 — FINDING (2026-08-08, Phase 3) — growth over an EMPTY cluster strands the old root

Found by the lost-CAS concurrency test, which grew a cluster nobody had mapped
into yet and tripped the validator's §6.4 check ("an empty non-root node
survived — reclamation did not reclaim").

**It is not a defect in growth; it is a gap between two rules that are each
correct.** §5.1 exempts the cluster root from reclamation — "the walk terminates
at a cluster root", because its parent slot is a bucket word with no state word
to acquire the interlock on. §6.4 reclaims a node when an operation *empties*
it: the candidate test is `clearsHere > 0 && observed == clearsHere`. A cluster
root that was **already empty** when growth pushed it below a new root satisfies
neither: it is no longer exempt, and it will never be emptied again because it is
already empty. `clearsHere` is zero for it forever.

**Reachable in production**, not only in a test: an address space that unmaps a
cluster completely — the root survives, by the exemption above — and then makes a
fixed-address request needing a wider span grows over an empty root.

**Bounded and small.** At most one stranded node per growth, and growth is capped
at `levelCount − defaultRootLevel` steps per cluster, so **two nodes per cluster**
at the amd64 default. It is also recoverable rather than permanent: the next
operation that maps and then unmaps through the stranded node reclaims it by the
ordinary path, which the test asserts.

**Taken: recorded, not fixed.** The fixes all cost more than the residue:

- *Reclaim the old root during growth.* It is reachable by concurrent descents
  at that moment, so this needs the claim protocol — and the interlock does not
  exist until after the CAS publishes the parent, which is precisely the ordering
  that makes it hard. A two-step "grow, then reclaim" is possible but adds a
  second publish to the one operation §5.6 keeps to a single CAS.
- *Skip growth when the cluster is empty and re-create at the covering level
  instead.* This replaces a node that concurrent readers may be descending, which
  is a reclamation in disguise and has the same problem.
- *Have the placement that motivated the growth reclaim it on the way past.* The
  cheapest of the three and still a special case in the commit walk for a
  two-node saving.

**Spec follow-up owed**: DEC-096's residue bound should carry this term
explicitly, and §6.4's walk-termination paragraph should note that the exemption
is positional — a node that *was* a cluster root is an ordinary node afterwards,
and the transition leaves this hole.

Pinned by `radix_growth_over_an_empty_cluster_leaves_the_old_root_behind`, which
asserts the residue is exactly one node **and** that the ordinary path recovers
it. If someone later teaches growth to reclaim, that test fails and gets deleted
deliberately rather than drifting.

---

## D-031 — DEFECT (found by the placement histogram, fixed) — "empty" must mean the RANGE, not the slot

DEC-007's fused verify-is-install needs an emptiness verdict, and the first
implementation judged it over the slot **word**: any non-empty word meant
occupied. That is the natural reading of "place only into empty space", it
round-trips through every single-placement test, and it is wrong by a factor of
sixteen.

**The shape it rejects.** A C1 slot holding a leaf over its first 64 KiB has
fifteen more granules free. The dispatch row for a placement into one of them is
`Subdivide`, which builds a child holding the survivor *and* the new leaf — it
ADDS alongside rather than displacing. The word is a leaf, so a word-based test
calls it occupied.

**How it presented**: the exit gate's probe-retry histogram filled **32 of 512
granules** in a 32 MiB cluster — exactly one per C1 slot — and then reported
`kNoSpaceInCluster` with 94% of the cluster empty. Note what that failure looks
like from outside: not a crash, not a wrong mapping, just an address space that
runs out of room early and grows more clusters than it needs. Without a test that
counts fill against capacity it is invisible.

**Fixed**: a leaf is occupancy only where it **overlaps** the requested range,
compared against the decoded absolute range (never the raw word — §3.1's
level-relative trap applies here as much as it does to the validity token). A
child word that did not dispatch to a descend is a fully-covered subtree and is
treated as occupied outright.

That last clause is deliberately conservative and worth naming: D-030's stranded
empty node would be judged occupied, so a placement can decline a range that is
in fact free. Conservative is the right direction — the cost is one probe moving
on, where the opposite error is a lost mapping.

**Second defect, same test**: the scan loop conflated a chunk's two false
returns. `findFreeRunChunk` returns false both for "no run within this chunk's
64-slot budget, call again" and for "the scan reached the end", and only the
cursor distinguishes them. Looping on the return value stopped every scan 64
slots in — again presenting as a cluster reporting itself full. The loop now runs
on `!cursor.finished()`.

**Both were found by the same test, and neither by any correctness check.** The
histogram exists because DEC-095 wanted a measurement; it earned its place by
catching two bugs that no assertion about mappings would have seen.

---

## D-032 — RESOLVED 2026-08-08 (same session) — the teardown walk is now DEC-100's

§7.4's SEQUENCE is implemented in its stated order (`destroyAddressSpace`), and
the phase plan's warning that "every reordering reviewed was a fatal" was taken
literally: dying flag → thread destruction (the caller's) → `synchronize` → the
walk → `drainAllQuiescent` → free the root page and the pools → `deinit` → return
the control block.

**What is not yet DEC-100's** is the walk itself. DEC-100 asks for a
unit-decomposed walk — each cluster torn down as a series of units that claim,
mark, unlink and **retire**, with the final unit clearing the bucket word and
retiring the root inside its own read section. What is implemented is the
Phase-1 synchronous release: a post-order walk running each node's deleter
directly, which is correct here because `synchronize` plus thread destruction has
made the walk genuinely unobserved.

**Two consequences, one of which required a deviation from the stated sequence.**

**(a) An extra drain, before the walk.** Under DEC-100 every `Mapping` release at
teardown — the ones riding node deleters and the ones riding `DeferredRelease`
records — happens inside the single `drainAllQuiescent` that follows the walk, so
each record's count reaches zero exactly once and the order among them does not
matter. With a **synchronous** walk it does: a record still sitting in a bag from
an earlier operation would release a `Mapping` the walk has already destroyed at
count zero. So `destroyAddressSpace` drains **before** the walk as well, which
empties those bags while the tree is still whole. Nothing runs between the two —
the threads are gone — so no new record can appear. The extra drain disappears
when the walk becomes retire-based.

**(b) No marking.** DEC-100's walk marks every node it unlinks, and §7.4 says why:
"Without any marking at all, teardown would be the only unlink path setting no
mark, leaving a foreign CPU's surviving cache entry pointed at a node that is
unlinked, unmarked, alive, and holding slots that point at freed children." That
hazard is **Phase 4's**, because the descent cache does not exist yet — there is
no surviving cache entry to mislead. It becomes live the moment DEC-016's cache
lands, and the cache's `resumeDescent` contract depends on teardown's marking to
make `Detached` double as the address-space-gone answer.

**Scheduled, not deferred indefinitely**: the unit-decomposed walk is a Phase 3
work item that is *partially* complete, and it is a **hard prerequisite for Phase
4**, not merely a tidy-up. Landing the descent cache over a non-marking teardown
is the exact defect §7.4 spends a paragraph on.

---

**RESOLUTION (same session).** `CoreTree::tearDownUnits` implements the walk:
each node is its own unit — one read section, whole-node claim, mark, unlink,
**retire** — with the parent-slot bit released at unit end while the node's own
whole-node mask never is (§7.4 is explicit that holding it "would sit in the
later unit's `fetch_or` prior and fire the quiescence assert on every process
exit"). The final unit clears the bucket word inside its own section and retires
the root like any other node; since DEC-103 there is no descriptor, so the
bucket's reference is released by the root's own deleter — one object, one
releaser.

Both consequences above are gone with it. **(a)** The extra pre-walk drain is
removed: every release now lands inside the single drain that follows, over
disjoint slot sets, so the count reaches zero exactly once whichever releaser
gets there last. **(b)** Every node is marked before it is unlinked, which is
the Phase 4 prerequisite.

The walk carries a node pointer across its children's section closes, which §7.3
forbids in general. Sound here for a reason §7.4 states rather than assumes: by
then the dying flag, thread destruction and `synchronize` have made the walk not
merely uncontended but **unobserved**. Nothing else in the tree gets to make that
argument, and the comment at the site says so.

Pinned by `radix_teardown_walk_marks_and_retires_rather_than_destroying`, which
runs §7.4's steps by hand so the window between the walk and the drain is
observable, and checks all three properties in it: the root is marked, the nodes
are still live objects (retired, not destroyed), and the bucket word is already
clear. A synchronous walk fails the second of those immediately.

---

## D-033 — CHOICE (2026-08-08, Phase 4) — the pin carries `{node, base, level}`, so an entry is 64 B and not 48

DEC-079 records the cache's honest footprint as **~192 B per CPU**: 4 × {8 B
generation, 16 B range, 8 B pin} = 128 B, plus 4 × 16 B per-entry candidate
registers = 64 B. Three cache lines, and the entry says so twice (the figure was
itself a round-5 correction of an earlier "~72 B").

The implementation is **256 B per CPU** — four lines. The difference is entirely
the **pin: 24 B, not 8**.

**Why it cannot be 8.** DEC-012 keeps level and base out of nodes: descent
derives a node's level by counting down from the root's and a subtree's base by
masking the search key. So a bare node pointer is not resumable — `resumeDescent`
needs the level to index the right bit field and the base to compute slot bases,
and `release` needs the level to pick the concrete type for `destroy<T>`
(DEC-062's level→type map). Nothing in the node supplies either.

**The packing that would have fit, and why it was rejected.** Nodes are 64 B
aligned by contract, so the level fits in the pointer's low bits; and every
node's span is naturally aligned, so `base` could be re-derived at resume time as
`key & ~(nodeSpan(level) - 1)`. That is 8 B exactly and reproduces DEC-079's
arithmetic.

It was rejected because **the derivation is silently wrong for a key outside the
node**, and the wrongness is exactly the class of failure the seam exists to
prevent: a bad key yields a plausible base, a plausible slot index, and a
`Mapping` for an unrelated address. The cache does check the key against the
entry's VA range first, so the derivation would be correct *today* — but it makes
correctness a property of the caller rather than of the handle, on the one API
whose whole job is to be safe to hold across a section close. `resumeDescent`
instead range-checks the key against the base it carries and answers
`OutOfRange`, which makes the seam total.

**What the extra line costs.** Nothing the spec depends on. The entry count is
DEC-079-Provisional; DEC-096's residue bound is stated in NODES
(`processorCount() × entries`) and is unchanged; and the per-CPU array is
machine-global rather than per-address-space, so at the 256-CPU architectural
maximum this is 64 KiB of BSS against DEC-096's own ≈320 KiB of pinned residue.

**Spec follow-up owed**: DEC-079's certainty column should carry the corrected
arithmetic and the reason, since it was written as a deliberate correction and a
third figure appearing without explanation would read as drift.

---

## D-034 — FINDING (2026-08-08, Phase 4) — the deferred install does more than DEC-079 claims, and threshold 1 inverts DEC-016

DEC-079 justifies the VA-range-deferred install with one benefit: "no atomic is
paid on first sight". The Phase 4 calibration sweep
(`radix_cache_calibration_report`) shows it does something else that is worth
more, and shows the alternative failing in a way the entry does not anticipate.

**The measurement.** Eight hot regions round-robin over a four-entry cache, so
every index is over-subscribed two-to-one, 64 rounds:

| threshold | hit % | installs | pin atomics | thrash |
|---|---|---|---|---|
| 1 (install on first sight) | 0.0 | 512 | 1020 | 508 |
| 2 (DEC-079's) | 0.0 | 0 | 0 | 0 |

Neither hits, because a direct-mapped entry genuinely cannot hold two alternating
regions — that is the pathology DEC-079 already names. What differs is the cost
of failing:

- **At threshold 1** every miss installs, so each access pays an acquire and a
  release. **Pin atomics scale with the LOOKUP count**, which is the precise
  inversion of DEC-016's cost claim that "atomics fall on cache turnover rather
  than on faults, so cost scales with miss rate". A workload that misses every
  time pays two atomics per fault forever.
- **At threshold 2** the two regions overwrite each other's candidate before
  either reaches the threshold, so the entry never installs and **the cache
  switches itself off for that index**: no atomics, no evictions, one generation
  compare per lookup. The cost of a hopeless index falls to nearly nothing.

So the deferral does not merely save one atomic on a cold range; it **bounds the
atomic cost of an over-subscribed index at zero**. That is a stronger argument
for the same decision, and it is the one to state, because it survives the case
where the entry's own stated benefit is irrelevant.

**Degradation is per-index, not global**, which is the other half of the answer:
five regions over four entries runs at **58% hits with 3 installs** — three
entries settle and the oversubscribed one switches off. The cliff is local.

**What this does NOT settle.** The threshold sweep on a *non*-over-subscribed set
cannot separate 1 from 2 (98.4% vs 96.9%, the difference being one warm-up miss
per entry), because the synthetic workload has no locality structure for the
deferral to exploit. **Entry count stays at DEC-079's provisional 4 and the
threshold at 2, with the real calibration explicitly deferred to Phase 5 data** —
which §13's Phase 4 gate permits in as many words ("entry count and install
threshold calibrated **or explicitly deferred to Phase 5 data**").

**Spec follow-up owed**: DEC-079's rationale should carry the over-subscription
result — both that threshold 1 inverts DEC-016's cost model and that the deferral
bounds a hopeless index at zero atomics.

---

## D-035 — CHOICE (2026-08-08, Phase 4) — the entry index granularity follows the pin level

DEC-079 says the set is "indexed by VA bits" and does not say which. The choice is
not free, and both wrong answers waste the set:

- index **finer** than the pinned node's span and one node is installed into
  several entries, each a redundant pin;
- index **coarser** and two distinct pinnable nodes collide on one entry
  permanently.

So the shift is derived from the cache's pin level rather than being a constant:
`nodeSpanBits(G, pinLevel)`, and `kPinAtDeepest` uses the geometry's floor level
(64 KiB under the amd64 default). One knob, not two, and the two cannot drift out
of agreement.

The residual imprecision is inherent and is worth naming: **with `kPinAtDeepest`
the pinned node's level varies with the tree's contents**, because leaves live at
any level. A region whose subdivision stopped at level 4 gets a 32 MiB entry
indexed at 64 KiB granularity, so 512 indices name one node. That is correct and
merely redundant, and no fixed shift can avoid it — the alternative is indexing by
something only known after the descent, which is not an index.

---

## D-036 — CHOICE (2026-08-08, Phase 4) — a new RCU veneer, `assertInReadSection`

§11 asks for "no reference is acquired outside the observing section" to be
**asserted**, not documented. The framework already has the check — `protect` and
`protectWord` call `detail::assertInSection` — but neither is reachable from the
pin, because a pin is a `fetch_add` on the referent and not a load of a link.

`kernel::rcu::assertInReadSection(const Domain&)` is that check exported as a
public inline veneer. Debug-only like everything behind it, and it compiles to
nothing when `CROCOS_RCU_DEBUG_CHECKS` is off.

The check is one-sided and the comment at the site says so: the framework can
answer "is a section open on this domain", not "is it the same section the link
was loaded in". What closes the other half is the shape of the API — a `PinSite`
is produced by `descendLocked` and consumed by `pinLocked`, both under one
`ReadGuard` — plus §11's own test case, which fills a candidate, closes the
section, reclaims the node, and requires the later install to have re-descended
rather than remembered (`radix_cache_candidate_survives_reclamation_of_its_node`).

**Cross-spec follow-up owed**: `specs/rcu.md` should record the veneer alongside
`protectWord` (RCU-DEC-044), which exists for the same reason one layer down — a
consumer-side bare check silently drops the framework's debug assert.

---

## D-037 — HARNESS NOTE (2026-08-08, Phase 4) — the oracle grows a destroy observer

§7.5 spends a paragraph establishing that the eviction's zero-observing
`destroy<Node>` **inside the descent's open read section, in `#PF` context** is
legal: it is the destructor and not a deleter, so §7.6's
deleters-run-outside-any-section rule does not govern it; invariant 24 makes the
destructor release nothing; vmsmalloc DEC-014 permits `vmsfree` from `#PF`.

Legality is a property of **where** the call happens, and "where" leaves no trace
in any counter. A test can assert that a destroy occurred, and that assertion
passes just as well on an implementation that quietly moved the release outside
the section — which is the natural refactor, since moving it there looks like
tidying.

`CroCOSTest::radix::setDestroyObserver` is the smallest thing that can answer it:
a callback from `noteDestroyed`, i.e. from inside `VMSubstrate::destroy`, which
asks `kernel::rcu::test::inSection` at that instant.
`radix_cache_eviction_destroy_runs_inside_the_descent_section` arranges a pinned
node whose structural count has already gone (unmapped, graced), then drives an
install that evicts it, and requires the destroy to be observed **inside** a
section.

---

## D-038 — TEST NOTE (2026-08-08, Phase 4) — a single page does not make a deep tree

Recorded because it cost time and because the failure mode is a test that passes
while measuring the wrong thing.

Leaves live at any level, so a tree's depth is a property of its **contents**. At
level 4 under the amd64 geometry the slot range field's unit is already 4 KiB
(`resolutionShortfall(GA, 4) == 0`), so a single 4 KiB mapping is written as a
leaf in a level-4 slot and the descent terminates two nodes below the cluster
root. The deepest node it visits spans **32 MiB**, not the 64 KiB of the
geometry's floor level.

The first version of the Phase 4 tests mapped single pages and asserted entry
spans against `nodeSpan(G, G.levelCount)` — 64 KiB. Three of them failed
outright; the ones that passed were passing against a 32 MiB level-4 node, which
is not what "an entry on the interior node above the mapping" means, and the
eviction tests silently became hit tests because two VAs 256 KiB apart were
inside the same 32 MiB node.

Every fixture that wants a floor-level node now maps a **pair of adjacent
pages**: two leaves inside one 64 KiB level-5 slot force subdivision to the floor.
The file carries a `static_assert` on the shortfall so a geometry change breaks
the build rather than the assumption.

---

## D-039 — DEFECT (found by the first in-kernel run, Phase 5) — §7.1's freshness sites were never implemented, and its list is incomplete

**The single most valuable thing Phase 5 has produced, and it landed on the first
boot.** It is also the DEC-047 precedent repeating exactly: a stale-TLB bug class
that the userspace harness is *structurally* incapable of seeing, because its
`ensureTLBEntryFresh` is a no-op and it has no page tables.

§7.1 states the rule and names three sites:

> "**Deleters touch bodies, and freshness is not free there** (RCU-DEC-006):
> `onPreTouch` covers each retire subject's `RetireHead` fields and nothing else,
> so every access a deleter makes beyond the head — a node deleter reading its own
> slots, either deleter RMW-ing a `Mapping`'s count word, the record deleter
> reading its own fields — is a cross-CPU access to vmsmalloc-backed memory and
> goes through the `SafePtr` / `ensureTLBEntryFresh` discipline." … "**The same
> discipline covers each record's first draw**."

Of those, **only the `DeferredRelease` pair was implemented** (the record deleter
and the pool pop, both already in `DeferredRelease.h`). The node deleter's slot
reads and the `Mapping` count-word RMW had no call at all. 150 userspace tests on
two sanitizers pass without them.

### What the target said

Four cycles of churn on 8 CPUs, then:

```
radix DIAG: releaseRefs prior=0 n=1 rec=0xffffff0000238100 baseVA=0xffffff0200202280
Panic: Assert failed: radix Mapping: release below zero
```

`prior=0` on a record whose `baseVA` is nonsense — the record's page had been
reclaimed and re-backed, and the releasing CPU was reading the previous tenant's
bytes through a stale mapping. Exactly the shape of vmsmalloc's own DEC-047.

Ruled out before fixing, rather than after: the stress's own
destroy-on-failure paths were disabled (`leaked=0`, `oom=0` in the liveness line
— neither had ever run), and concurrent growth was disabled. Both left the
failure unchanged.

### Two more sites the spec does not name

Fixing §7.1's two moved the failure, twice, and each move named a site the spec's
list omits. **Both are on the fault path**, which is what makes them worth
escalating rather than merely recording:

1. **The root bucket page.** `radix bucket codec: a non-zero entry without its
   guard bit`, at cycle 5. The table is a whole-page `tryMake<BucketTable>`
   allocation, so its VA recycles like any other arena page — and address-space
   creation/teardown recycles it every cycle. *Every descent* opens by loading a
   bucket word out of it. Now behind `BucketTable::fresh(i)`.

   **This is a design question, not just a missing call.** DEC-082 moved the
   control block into pinned `reservePerDomainStaticBuffer` storage specifically
   so that "the generation check, the `dying`-flag load and the pool heads —
   all of which sit on other CPUs' hot paths — carry **no freshness obligation at
   all**", and its own rationale rejects "an `ensureTLBEntryFresh` on every
   descent-cache hit". The root page is the one per-address-space allocation that
   did not move with the control block, and it is read *more* often than any of
   the fields that did. Either it belongs in the pinned block too, or DEC-082's
   argument needs to say why the root page is different. **Flagged for Spencer;
   the freshness call is the conservative answer in the meantime.**

2. **The reader's and writer's first touch of a `Mapping` body.** `radix Mapping:
   fault VA below the mapping base — the offset derivation would underflow`,
   i.e. `baseVA` read as garbage. `descendLocked` decodes a record from a slot
   word and does `m->acquireRef()`; `takeSubtreeReferences` does the same on the
   write path. Neither had a freshness call, and §7.1's list covers deleters
   only.

   The call goes at **reference acquisition**, and the placement is an argument
   rather than a convenience: one call there covers every later access the caller
   makes through that reference, because the counted reference is precisely what
   stops the record being destroyed and its page recycled underneath. DEC-073
   already says the obligation exists — "whatever pointer it derives carries the
   `SafePtr` freshness obligation exactly as with `protect`" — so this is §7.1's
   list being incomplete rather than the design being wrong.

### Spec follow-ups owed

- §7.1's freshness list should gain the reader/writer reference-acquisition site
  and the root bucket page, or state why each is exempt.
- DEC-082 should either extend to the root page or record why the root page's
  freshness obligation is acceptable where the control block's was not.

---

## D-040 — CHOICE (2026-08-08, Phase 5) — `SubRange` is zero-initialised, and LTO is what made that necessary

`-Werror=maybe-uninitialized` in the **Release/LTO** build only:
`SubRange::{lo,hi}` were uninitialised, and both writers pass one by reference to
the failable `Codec::subRangeFor`. One checks the result; the other
debug-*asserts* it ("a fully-covered child slot's replacement is the whole span,
which is always expressible"). In release the assert compiles out, so a false
return would encode a leaf from indeterminate stack — undefined behaviour rather
than the merely-wrong range the assert describes.

The members are now `= 0`. Worth recording because of *why it was invisible*:
without cross-TU inlining the release kernel could not see the call and the
warning did not exist. [[project_kernel_lto]]'s point again, from the other
direction — LTO is not only an optimisation, it is a diagnostic.

---

## D-041 — RESOLVED (page fault) / WATCH (record residue) — the two rare Release-only failures

**RESOLUTION (same session).** The page fault was the **reader-side `Mapping`
access on a migrated thread**, and it is gone: with `LookupResult` holding a
`SafePtr<Mapping>` and every touch of a record's body going through it,
Release/LTO is **8 runs out of 8 clean at 1025 cycles**, against 6 of 8 with
D-042's node fix alone and roughly 4 of 8 before either. See D-044 for the
mechanism — a freshness call made once at acquisition guarantees nothing to a CPU
that did not make it, and DEC-015 exists precisely so the reference can cross to
one.

The `Mapping` residue of 2 has not recurred since and stays a **watch item**
rather than a resolved one: it was seen once, the mechanism was never
established, and one clean sweep is not evidence of absence.

The original text follows.

---

## D-041 (original) — two rare Release-only failures, not yet diagnosed

Both appear only in the **Release/LTO** build, which runs ~250x more cycles in the
same 20 s window (1025 cycles vs 4), so they are rate-limited discoveries rather
than build-specific ones. Debug is green on every QEMU config; Release is green on
most runs. **Neither is diagnosed, and neither should be assumed benign.**

**(a) A page fault, roughly 1 run in 6 — SUBSTANTIALLY REDUCED by D-042's node
freshness fix (6 of 8 runs now clean at 1025 cycles), but not eliminated. The
remaining instance faults at the same `tearDownUnitAt` site on a pointer that the
freshness call did not save, i.e. a genuinely bogus child pointer rather than a
stale-but-valid one. See D-043 for the debug reproduction.**

```
Pagefault at 0xffffffffc015ba6e accessing 0xffffff0007ffffe0
```

The faulting address is inside CPU 0's arena (substrate base + 128 MiB − 32 B) —
32 bytes below a 128 MiB boundary, which is the shape of a read just past the end
of a mapped region or of a VA whose backing was released. The likeliest
candidates, in the order they should be checked: a fourth freshness site D-039
did not reach; a node or record touched after its page was returned by
`reclaimSlabPage`; or an arena-boundary case in vmsmalloc reached by this
workload's allocation mix and not by RCU Phase 4's.

**(b) `Mapping` residue of 2 after teardown, seen once (`run_numa_hmat`).**

```
radixStress: RESIDUE — 2 Mapping records live after teardown.
```

The node residue gate passed in the same run, so this is records specifically.
Two suspects worth separating before anything else: the stress's own
shallow-discard of a record whose placement failed (a decomposed `apply` that
returns `OutOfMemory` **has** committed earlier units, so the record may be
published — the stress notes this at the site and argues the row cannot
decompose, and that argument is worth re-checking); and a genuine deferred-release
record that never came home.

The residue gate is what found (b), which is the gate working: it is stated at
**zero** for records precisely because a leaked record pins its VMObject and every
frame behind it.

---

## D-042 — DEFECT (Phase 5, fixed) — a node pointer decoded from a slot word owes the freshness call too — **RESOLVED 2026-08-09, option 1**

D-039's third instalment, found the same way and one layer deeper. Under
Release/LTO — which reaches **1025 cycles** in the same 20 s window a debug build
spends on 4 — the stress page-faulted about one run in four. Two faulting sites,
both resolved by name rather than guessed:

```
Pagefault at 0xffffffffc015ba6e accessing 0xffffff0007ffffe0
  -> CoreTree<...>::tearDownUnitAt   (CoreTree.h:677, `node.slot(i).load`)

Pagefault at 0xffffffffc015b559 accessing 0x000000ffffe00f80
  -> VMSubstrate::ensureTLBEntryFresh(void*)
```

The second is the first one step later: a stale slot read yields a garbage
`Mapping*`, and the freshness call D-039 added then faults *on its own argument*.

**Nodes are vmsmalloc allocations**, so a node's VA recycles like any other arena
page, and a CPU that used that VA under a previous tenant holds a stale mapping.
DEC-073 already states the rule — a link load goes through `protectWord` and
"whatever pointer it derives carries the `SafePtr` freshness obligation exactly
as with `protect`" — and a node pointer decoded from a slot word is exactly such
a pointer. §7.1's list is about deleters and never generalises it.

`NodeRef::fresh(void*)` is the fix, applied at the seventeen sites where a node
pointer is **derived** rather than at every field read: the page cannot be
recycled again while an operation is standing on the node, so one call per node
per walk suffices.

**Result: 6 of 8 Release runs now reach 1025 cycles cleanly, against roughly half
before. It is an improvement, not a fix, and the remainder is D-041.**

### The cost, which was not priced anywhere (it is now — see the resolution)

This is **one `ensureTLBEntryFresh` per level on the descent** — the spec's
hottest path — and RCU P4-ITEM-002 measured a freshness call at ~40 instructions
pre-LTO. DEC-082 rearranged an entire control block to keep *one* such call off
the descent-cache hit path; this adds one per level to every descent.

Three answers, and choosing between them is Spencer's:

1. **Keep the calls.** Correct, and the cost is now measurable — Phase 5's
   `-icount` work can price it directly.
2. **Nodes come from storage that never recycles.** DEC-082's own answer for the
   control block, applied one level down. Removes the obligation outright rather
   than paying it, and would take the root bucket page (D-039) with it.
3. **vmsmalloc discharges it.** `reclaimSlabPage` already leaves a reclaimed VA
   mapped read-only onto a sentinel rather than unmapped, precisely so a
   mid-flight Treiber pop reads garbage instead of faulting; whether that
   guarantee can be strengthened into "a re-backed slab page is globally fresh"
   is a vmsmalloc question, not a radix one.

### RESOLVED 2026-08-09 — option 1, keep the calls (Spencer)

Decided against measurements rather than against the estimate the entry above
was written with, which turned out to be both right and wrong in instructive
ways. The evidence, in the order it was gathered:

- **D-052** (harness, exact): the read path makes **1.00 freshness calls per
  level**, plus one for the record. The estimate in this entry survived D-044's
  move from per-derivation to per-access untouched, because a lookup reads
  exactly one field per node — the slot it descends through. The write path pays
  7–11 per level, but that is `mmap`/`munmap`, which already allocates and locks.
- **D-052** (harness, best case): a descent's calls land on **2–3 distinct
  pages** even at depth 6, because same-size-class nodes share a slab page. Since
  the call `invlpg`s only when the page's dirty bit is set, **only the first call
  per page can miss** — which is the specific worry this entry raised, and it is
  much weaker than "a cache line per node".
- **D-053** (target, `-icount`): **24 instructions per call** net of probe — not
  the ~40 quoted above, which was pre-LTO — against **516–797 for a whole
  lookup**, at **3–5 calls per lookup**. So the discipline is **~10–20% of the
  fault path's instruction count, ≤23% on a full cache-missing descent**. For
  scale, the same call is 51–59% of an RCU retire.

**What option 1 accepts, stated plainly**: the cost of a call that *misses* is
still unmeasured, because TCG models neither the cache miss on the dirty-bitmap
word nor `invlpg`. What makes that acceptable rather than an open item is the
concentration finding — the number of possible misses per descent is bounded by
the number of distinct pages a descent touches, not by its depth.

**What this does not close.** Options 2 and 3 remain the right answers if the
cost is ever shown to matter, and D-051's `BlockPool` backend seam is where
option 2 would now be built. Nothing about this decision blocks them; it declines
to pay their complexity on the strength of a cost that measured small.

The §7.1 spec text is still owed, now including this entry's resolution.

---

## D-043 — RESOLVED (Phase 5) — `klog` is not reentrant, and an interrupt-context log can self-deadlock against one in progress

**Not a radix defect.** A pre-existing kernel-wide hazard that the stress's klog
rate makes likely, and the stack trace names it exactly:

```
Core::AtomicPrintStream::AtomicPrintStream(PrintStream&)      <- re-acquires
kernel::enqueueShutdown()::lambda                             <- klog("Goodbye :)")
kernel::timing::dispatchTimerEvent()
arch::amd64::interrupts::LAPICTimer::executeCallback(...)
kernel::interrupts::managed::dispatchInterrupt(...)
```

`AtomicPrintStream` holds **one global `Spinlock` for the whole lifetime of the
temporary**, so a `klog()` expression is a critical section spanning every `<<`
in it. The shutdown timer fires on a CPU that is mid-way through one — the log
cuts off at `rcu: domain [radix-as] ready — 8 slots across 1 ` — its callback
klogs, and the same CPU re-acquires a lock it already holds.

So **any `klog` from interrupt context can self-deadlock against a `klog` in
progress on the same CPU.** Nothing about this is specific to the radix tree; the
stress merely makes it probable, by driving ~1025 address-space creations per
boot and therefore ~1025 `Domain::init` log lines on CPU 0, which gives the 20 s
shutdown timer a good chance of landing inside one.

Two things worth noting beyond the fix:

- **The detector saved this run.** `SUPPORTS_SPINLOCK_DEADLOCK_DETECTION` is
  debug-only, so in a release build the same interleaving is a silent hard hang
  with the print lock held — and the hang watchdog cannot report it, because the
  watchdog is itself a timer event on a CPU that is now spinning in an ISR.
- **The fix is not the radix tree's to choose.** The options are a per-CPU
  reentrancy allowance on the print lock, masking interrupts for the duration of
  a `klog` expression, or deferring interrupt-context logging to a ring the way
  `HighReliabilityRingBuffer` already permits. **Flagged for Spencer.**

Reproduction: `-DCROCOS_RADIX_STRESS=ON -DCROCOS_RADIX_STRESS_OPS=24`, debug,
`-smp 8`, about one run in six.

### RESOLUTION (user-directed, same session)

Spencer's call: the shutdown message uses the raw logger, and the hazard gets
written down where it will be read.

**The rule, now documented at `klog()` in `kernel.h`, at `emergencyLog()` in
`panic.h`, and at `AtomicPrintStream` itself**: `klog()` returns a temporary
holding a global spinlock for its whole lifetime, so `klog() << a << b << c;` is
one critical section spanning every `<<`. That is what makes a line atomic
against other CPUs and it is exactly why the call is not reentrant. **Timer
callbacks, interrupt handlers and panic paths use `emergencyLog()`** — the
unlocked stream, whose cost is that concurrent writers interleave at byte
granularity. That trade is the right way round: interleaved output is legible, a
deadlocked log is not.

Worth stating once, because the name misleads: `emergencyLog`'s useful property
is **lock-free**, not "for crashes". The panic path was merely its first
consumer.

**Six sites converted**, all of them genuine instances rather than the one that
happened to fire:

| Site | Context |
|---|---|
| `enqueueShutdown`'s `Goodbye :)` | timer callback — the one that fired |
| `smp.cpp`'s "All N processors up!" | timer callback |
| `rcuStress::reportHangAndExit` | watchdog timer callback |
| `radixStress::reportHangAndExit` | watchdog timer callback |
| `dispatchInterrupt`'s level-trigger warning (x3) | **inside the ISR** |
| `dispatchInterrupt`'s "no handler for vector" | **inside the ISR** |

The two watchdogs matter more than they look: a watchdog that hangs instead of
reporting is worse than no watchdog, and its entire job is to be the last thing
that still works.

`klog()` in `enqueueShutdown`'s own body is deliberately left alone — that runs
as an init routine, not in the callback.

**Verified: 12 of 12 clean on the reproduction that used to fail one run in
six**, plus debug and release green on all three QEMU configs and the full
userspace suite.

The original entry follows.

---

## D-043 (original) — a spinlock SELF-deadlock in the address-space lifecycle, with a reproduction

Shrinking `kOpsPerCycle` from 512 to 24 turns the debug build's 4 cycles into
1025 and makes D-041's Release-only failures reproducible **in a debug build with
every assert live** — which is the position any further work on this should start
from, and is why the constant now carries the recipe in its comment.

What it produced, roughly 1 run in 4:

```
rcu: domain [radix-as] ready — 8 slots across 1 page(s), dr
Panic: Assert failed: Deadlock detected!
```

`Spinlock`'s detector is **not a timeout** — it fires when the acquiring CPU
finds its *own* id in the lock's metadata, i.e. a genuine self-deadlock — and the
message lands mid-way through `Domain::init`'s own log line, so the second
acquisition is inside domain initialisation.

That is DEC-101's named trap ("`Domain::init` takes the same lock *internally*;
holding it across both steps self-deadlocks") — but `createAddressSpace` does
scope its `DomainManagementLockGuard` to step 1 and release it before step 2, and
a deterministic double-acquire would not be intermittent. So the likelier
explanations are elsewhere and should be separated before anything is changed:

- a second lock entirely (the VMSubstrate reservation path, or vmsmalloc), taken
  twice on one CPU along a path only this workload reaches;
- a `Domain::init` / `deinit` cycling defect: **this stress runs ~1025 domain
  create/destroy pairs per boot, a pattern RCU has never been asked to do** — its
  own Phase 4 stress creates one domain and keeps it;
- a false positive in the detector's metadata handling under rapid re-acquisition
  by different CPUs.

The third is the cheapest to rule out and should go first.

**Not diagnosed. Debug reproduction: `-DCROCOS_RADIX_STRESS=ON`, debug build,
`kOpsPerCycle = 24`, `-smp 8`, about one run in four.**

---

## D-044 — CHOICE (2026-08-08, Phase 5, user-directed) — the freshness discipline is `SafePtr`, and the API grew to fit

D-039 and D-042 fixed five freshness sites with **bare `ensureTLBEntryFresh`
calls**. Spencer's correction, and it is the right one: DEC-028 says "every read
of allocator-returned memory goes through `SafePtr<T>`", the VMM is `SafePtr`'s
only consumer, and where the type could not express a site the answer was to
widen the type rather than to reach past it.

### What was wrong, not merely unidiomatic

The bare calls at the deleter sites were ugly. **The one on the reader path was
incorrect**, and it is what D-041's page fault was:

`descendLocked` called `ensureTLBEntryFresh(m)` once, at reference acquisition,
justified by "one call covers everything the caller does through the reference,
because the reference stops the page being recycled". The second half is true and
the first does not follow. **Freshness is a property of ONE CPU's mapping.**
DEC-015 exists so the fault path can close its section and block on a userspace
pager — after which the thread can resume on a *different* CPU, which may hold a
stale entry for the record's page from a previous tenant and never made the call.
`LookupResult::mapping()` then handed out a raw `Mapping*`, so the VMM's
`baseVA` / `objectOffset` reads were unguarded on precisely the path the whole
feature was built around.

Measured: Release/LTO goes from 6-of-8 clean (D-042 alone) to **8-of-8 at 1025
cycles**.

### What the API gained

- **`T* address()`** — the typed address, discharging nothing. Named apart from
  every accessor so "this one genuinely reads no bytes" is sayable in the
  vocabulary: identity comparison, encoding into a slot word, the concrete-type
  cast `retire` and `destroy` need.
- **`at<U>(byteOffset)`** — a typed sub-object reference, made fresh on **its
  own** page rather than the base's, which matters for anything that can straddle
  a page and costs nothing when it cannot.
- **`SafePtr<void>`** — the type-erased form, whose motivating consumer is
  `NodeRef`. A descent stands on nodes of mixed valence and cannot name a
  concrete `Node<G, V>` (DEC-062's level→type map exists so it does not have to),
  so `SafePtr<Node<...>>` cannot be formed — yet the obligation is identical.
  Without this the only way to write the descent is a bare `ensureTLBEntryFresh`
  plus a `reinterpret_cast`, which is exactly the unguarded shape the type exists
  to prevent.
- **Hidden-friend comparisons**, so a raw pointer on either side works. An
  identity check that had to be spelled `a.address() == b` would push callers
  towards holding raw pointers, which is the habit being broken.
- A default constructor and a move assignment, so `LookupResult` can hold one.

### The collapse this forced, which is the useful part

`releaseMappingRefs` previously had a `...FromDeleter` variant that made the
freshness call and a plain one that did not, split on the reasoning that §7.1
names deleters only and that a call on the reader's release would land on the
fault path. **The split is gone.** A release is an RMW on the record's count
word, and by the migration argument above the reader's release is in exactly the
position the deleter's is. The only reason it looked different is that §7.1's
list was written about deleters. One entry point now, and `acquireMappingRef`
alongside it for the same reason.

### Cost, still unpriced

Per-access rather than per-pointer means the descent pays one call per level
(`NodeRef::slot`), the writer paths pay one per state-word touch, and the reader
pays one per record access. D-042's three options are unchanged and this makes
choosing between them more urgent, not less.

### One test repaired in passing

`radix_concurrent_subtree_replacement_is_atomic` had a start-order race of its
own: the replacer's 200 whole-slot replacements can finish before a reader thread
is scheduled, and the reader loop consulted `stop` before its first sweep, so it
made zero observations and failed its own vacuity check. It now completes one
full sweep before checking. A test bug, not a tree bug, but one that fires
whenever anything shifts the timing — which this change did.

---

## D-045 — DEFECT (2026-08-08, user-directed) — the release pools were sized for 256 CPUs on every machine, and the freelist threw away NUMA placement

Two independent problems in the same structure, found by measuring the control
block rather than by reading it. Both are about the **pinned static-buffer
window**, which is the scarce resource here: one 1 GiB region, bump-allocated,
with no free path at all.

### (a) `DeferredReleasePools::pools[arch::MAX_PROCESSOR_COUNT]`

The pool array was a by-value member sized at the compile-time processor cap —
256 cache-line-aligned entries, unconditionally. Measured:

| | before | after |
|---|---|---|
| `sizeof(ControlBlock)` | 16,704 B | **256 B** |
| — of which the pool array | 16,448 B (98.5%) | 0 (moved to the tail) |
| reservation, 8-CPU machine | 16,704 B | **768 B** |
| reservation, 256-CPU machine | 16,704 B | 16,640 B |
| window ceiling (concurrent address spaces) | ~44,000 | ~500,000 |

> **CORRECTED by D-046.** The window figures in this table are wrong: reservations
> are rounded up to whole pages, so the true reduction is 20,480 B -> 4,096 B
> (5x) and the ceiling goes ~43,700 -> ~131,000 (3x), not ~500,000. The byte
> figures above are right; they were measured against the request rather than
> against the window.

On an 8-CPU desktop — the stated primary target — 98% of every process's pinned
reservation was padding for CPUs that do not exist. **21.75x smaller**, and
unchanged on a machine that genuinely has 256.

The array is now runtime-sized and lives in the **tail of the control block's own
reservation**, which is the part that is a decision rather than an
implementation detail. It could not simply become its own allocation: the pool
heads sit on every other CPU's hot path (every CPU's deleters push into every
CPU's pool), which is exactly why DEC-082's round-4 amendment put them in pinned
storage. A vmsmalloc array would hand back the `ensureTLBEntryFresh` obligation
that amendment removed. Keeping it in the same reservation preserves "one block
anchors everything" *and* the freshness-free property.

Consequences worth stating: `ControlBlock` is now variable-sized, so the freelist
is size-aware (`takeLocked` refuses a block whose trailing array is too short —
never reached in the kernel, where `processorCount()` is a boot constant, but the
harness varies it and an under-sized block would be an out-of-bounds write with
no diagnostic); and the zero-fill on reuse covers the whole reservation, not
`sizeof(Block)`, because a stale pool head is a live record list handed to a
process that does not own it.

### (b) `ControlBlockFreelist` discarded the NUMA domain

`createAddressSpace` takes a `home` domain and `tryReservePerDomainStaticBuffer`
honours it — **on the first reservation only**. One undifferentiated list meant
every reuse handed back whatever was at the head, so placement was a suggestion
obeyed exactly once. On a multi-socket machine that puts a process's control
block on a remote node, and the block holds the generation and the pool heads
every CPU reads on hot paths.

The list is now partitioned by domain, with each block recording where its pages
actually are (`homeDomain`) so `returnLocked` files it back where it came from.
Reservations are kernel-lifetime, so a block's placement is fixed at its first
reservation forever; remembering it is the only thing the freelist can do.

The cross-domain fallback is deliberate: a wrong-domain block beats burning
window on a fresh reservation, because the window is bounded and unreclaimable —
remote-but-present is a performance answer where exhaustion is a correctness one.

**`kernel::numa::kMaxDomains`** is a promotion, not a new constant: it lived in
vmsmalloc's implementation-internal `VMSubstrateSlab.h`, and a cap on `DomainID`
belongs with `DomainID` once a second subsystem needs it. vmsmalloc keeps its own
spelling as an alias, so no call site there changed.

### Confirming release + reuse, rather than assuming it

Reuse was only ever covered *incidentally* — the recycle test asserts the same
address comes back, and the in-kernel stress does ~1025 create/destroy cycles a
boot. Neither touches the mechanics this change introduced. Worse, **the harness
is structurally unable to catch the failure**: the mock's static-buffer
reservation is a page-granular bump pointer into one mmap region, so a 768-byte
reservation occupies a whole 4 KiB page and an overrun of up to 3.3 KiB lands
inside it — invisible to ASan, and silent until the day it reaches the next
block. Sanitizer trust is not available here; assertions are.

Three tests, each mutation-tested per D-022's rule:

| Test | Mutation | Caught |
|---|---|---|
| `..._freelist_preserves_numa_placement` | pool every domain onto one list | **yes** — the domain-0 request gets the most recently freed block instead |
| `..._freelist_refuses_an_undersized_block` | drop the `reservedCpus >= cpus` guard | **yes** — an 8-CPU request takes the 1-CPU block |
| `..._pool_array_lies_inside_the_reservation` + `..._recycled_block_pools_carry_record_traffic` | place the array overlapping the block's tail instead of after it | **yes**, by 7 tests — the overlap corrupts `reservedCpus` and `freelistNext`, which is exactly the silent-corruption mode |

The recycled-block test is the one that matters most: it drives real record
traffic (24 map/unmap pairs, each displacement drawing a `DeferredRelease` record)
through pools that live in the previous tenant's storage, and leans on
`pools.destroy()`'s population-conservation assert as the detector for a record
drawn and never returned.

**One mutation was NOT caught, and it is recorded rather than fixed**: shrinking
the reuse zero-fill from the whole reservation to `sizeof(Block)` changes nothing
observable, because `pools.destroy()` already drains every record and resets each
pool's head, population, depth and counters before the block is returned. So the
trailing array is clean on arrival and the wider `memset` is **defence in depth,
not currently load-bearing**.

It is kept for vmsmalloc DEC-051's reason — zero-fill is a per-RESERVATION
guarantee, not a per-block one — and because making cleanliness a property of
*creation* rather than of `destroy()` happening to stay thorough is what stops a
field added to the pool later, and not reset there, from becoming a live record
list handed to a process that does not own it. A test for it would have to poke
the array directly, which tests the `memset` rather than a behaviour; the comment
at the site says all of this so nobody later "simplifies" it back.

### What this does NOT fix

`Domain::init` reserves `divideAndRoundUp(cpuCount, kSlotsPerPage)` pages for its
reader slots — a **whole 4 KiB page** for 8 CPUs' slots. With the radix block down
to 768 B, RCU's slot page is now the dominant per-address-space term by 5x and
sets the real window ceiling. Same shape of problem, different subsystem, and not
touched here.

### And it reopens D-039's root-page question with different numbers

Folding the 4 KiB root bucket page into the control block was +24% when the block
was 16.3 KiB. Against 768 B it is **+533%**. The argument for doing it is
unchanged and is not about size — the root page is the only per-address-space
structure whose *mapping* churns, and that churn is what produced D-039's stale
bucket word — but the arithmetic now cuts the other way and the decision should
be taken against these figures, not the old ones.

---

## D-046 — CORRECTION to D-045's figures, and the note it points to

**D-045 overstated its own result**, and the error is worth keeping because it is
the kind that survives review: the numbers were measured, they were just measured
against the wrong quantity.

`reserveStaticBufferImpl` advances its bump pointer by **whole pages** —
`staticBufferNextVA += pages * smallPageSize` — so every reservation consumes at
least 4 KiB regardless of `byteSize`. D-045 reported the reduction in the *request*
and called it a reduction in the *window*.

| | before | after | |
|---|---|---|---|
| `sizeof(ControlBlock)` | 16,704 B | 256 B | as reported |
| reservation request, 8 CPUs | 16,704 B | 768 B | as reported |
| **window actually consumed** | 20,480 B (5 pages) | **4,096 B (1 page)** | **5x, not 21.75x** |
| ceiling (with RCU's page) | ~43,700 | **~131,000** | **3x, not ~11x** |

The fix is real and worth having. It is 3x, not an order of magnitude.

### Which is why the RCU reader-slot sizing was NOT done

`sizeof(ReaderSlot)` is 128 B, so 8 CPUs need 1,024 B and take a whole page — 75%
wasted. But **reserving 1,024 instead of 4,096 consumes the same page**. The waste
is structural in `reservePerDomainStaticBuffer`, not in RCU's request, and a
change there would be a no-op dressed as an improvement.

Also checked and **not** a bug: RCU's `gFreeSlotBlocks` does not lose NUMA
placement the way radix's freelist did. RCU derives placement from the fixed CPU
topology (`homeDomain(p * kSlotsPerPage)`), so every block has identical per-page
placement and blocks are interchangeable. Radix's differed only because `home` is
a caller-supplied per-address-space parameter.

### The note

The real fix is a **per-NUMA-domain pinned allocator with sub-page suballocation
and free/reuse**, recorded at `tryReservePerDomainStaticBuffer` in
`kernel/include/mem/VMSubstrate.h` where an implementer will find it.

The part that makes it tractable, and that took a wrong turn earlier in this
session to establish: **it must never unmap a page, and it does not need to.**
DEC-051b's safety argument is that every entry there "transitions not-present ->
present EXACTLY ONCE and never changes", which is exactly why pinned storage
carries no `ensureTLBEntryFresh` obligation. Reusing a *block inside an
already-mapped page* touches no PTE — the VA to physical mapping is unchanged and
only the bytes differ, so a CPU's cached translation stays correct. Free-and-reuse
is free. Only returning pages to the PageAllocator would break the invariant, and
no consumer has asked for that.

Evidence that it is owed: two consumers have now hand-rolled the same freelist
over these blocks (`kernel::rcu::gFreeSlotBlocks` and radix's
`ControlBlockFreelist`), and the radix one needed NUMA partitioning and a size
check the RCU one does not. A third consumer will write it again and get a
different subset right.

---

## D-047 — `PinnedBlockPool`: the allocator layer D-046 asked for (user-directed)

Spencer's call, and the framing was his: an allocator layer on top of
`reserveStaticBufferImpl` for handing out control-block-shaped memory, which
"doesn't need to be very fast — really it can just be a big linked list of
control blocks with a lock, if need be."

`kernel/include/mem/PinnedBlockPool.h`. A fixed-stride, per-NUMA-domain,
intrusive freelist over page-granular reservations, carving each page into as
many blocks as it holds.

### What it bought

| | before pools fix | after pools fix | with the pool |
|---|---|---|---|
| radix block, window cost | 20,480 B (5 pages) | 4,096 B (1 page) | **819 B** (5 blocks/page) |
| RCU reader slots | 4,096 B | 4,096 B | 4,096 B (unchanged) |
| **total per address space** | 24,576 B | 8,192 B | **4,915 B** |
| ceiling (concurrent) | ~43,700 | ~131,000 | **~218,000** |

Radix's hand-rolled `ControlBlockFreelist` is gone, and three things went with it:
the per-domain partitioning it had to grow after D-045, the size check that
existed only because variable-sized blocks made an undersized reuse an
out-of-bounds write, and the page minimum. What stayed behind is the
**generation**, which is an address-space identity concept (DEC-082) rather than
a property of the storage.

### Two decisions worth reading

**The pool never zeroes.** RCU-DEC-043(i) already states the rule better than a
new one could: zeroing at the consumer "makes every init valid regardless of
block provenance, which is the only form of the rule that does not depend on how
the caller obtained its storage." Zeroing here would make consumers' correctness
depend on which allocator they happened to use. Both existing consumers already
re-zero; radix's is now unconditional rather than only-when-recycled, because
with the pool owning reuse the caller cannot tell the difference and should not
have to.

**The cross-domain fallback is the LAST resort, not the second choice — and a
test caught that it was written the wrong way round.** The obvious ordering is
preferred list, then other domains, then carve, on the reasoning that reusing a
remote block beats burning window. That is wrong once blocks are packed: a carve
yields five blocks at once, so the very first request for a second domain finds
the first domain's spares and takes one, and the machine **never carves on its
second domain at all** — every address space after the first page is remotely
placed. It is not even a saving, since that domain gets carved eventually
regardless. Correct order: preferred, carve on preferred, then other domains,
which is reached only when the window is genuinely exhausted.

That reordering is why the fallback test now arms a scripted reservation failure
rather than merely emptying a domain's list: the fallback is only reachable at
the condition it exists for.

### Mutation-tested

| Mutation | Caught by |
|---|---|
| carve yields one block instead of packing the page | `..._store_packs_blocks_into_a_page` |
| no cross-domain fallback at all | `..._store_falls_back_across_domains_only_when_exhausted` |
| (earlier, D-045) pool every domain onto one list | `..._preserves_numa_placement` |

### Still owed

**RCU's reader-slot block is now the dominant term by 5x** and is deliberately
untouched. It does not fit this pool as written, for two reasons that are worth
recording rather than rediscovering: it places page `p` on the domain of the CPUs
whose slots that page holds, and it **depends on consecutive reservations
returning consecutive VAs** — a property of the bump allocator that a shared pool
breaks by interleaving carves. A single-page slot block (any machine up to 32
CPUs) has neither problem and could use a pool; the multi-page case needs either
a contiguous-extent request or for the contiguity dependency to be removed first.

*Settled by D-048 below, along the line this paragraph predicted.*

---

## D-048 — RCU's reader-slot block, packed (the item D-047 left owed)

`sizeof(ReaderSlot)` is 128 B, so an 8-CPU machine's slot array is 1,024 B and
took a dedicated 4 KiB page — **75% of it wasted, on every address space**, which
after D-047 made it 5× the radix control block and the dominant per-address-space
cost. Four now share a page.

| | before D-045 | after D-047 | now |
|---|---|---|---|
| RCU slot block, window cost | 4,096 B | 4,096 B | **1,024 B** |
| radix block, window cost | 20,480 B | 819 B | 819 B |
| total per address space | 24,576 B | 4,915 B | **~1,843 B** |
| ceiling (concurrent AS) | ~43,700 | ~218,000 | **~580,000** |

### The branch, and why it is honest rather than lazy

Both of the obstacles the "Still owed" note names are **statements about pages**,
and both go vacuous at one page:

- *per-page NUMA placement* degrades to "pick a domain" — there is no second page
  to place differently, and the code already picks CPU 0's domain;
- *the contiguity dependency* is a claim about consecutive reservations, and
  there is exactly one.

So the single-page case takes a `PinnedBlockPool` and the multi-page case keeps
the original path verbatim — whole-page reservations, contiguity checked,
whole blocks recycled through `gFreeSlotBlocks`. `cpuCount <= kSlotsPerPage` is
32 CPUs, i.e. the entire consumer-desktop target, so the kept path is the one
that never runs on the hardware CroCOS aims at. Unifying them would need a
contiguous-extent request from the pool — the one thing `PinnedBlockPool`'s
header says it does not do — which is a larger change than this item.

### Two decisions inside it

**Provenance is recorded, not re-derived.** A block must go back to whichever
source produced it, so `Domain` stores the DomainID beside `slotBlock`. It could
instead recompute `cpuCount <= kSlotsPerPage` at deinit, which works today and
silently misfiles every block the moment CPU count can differ between init and
deinit. The DomainID doubles as the tag using its own null sentinel: a pooled
block sits on one domain and names it (which `freeLocked` requires), and a
multi-page block spans per-page placements and has no single domain — one fact,
one field, no way for two fields to disagree. A block misrouted to the pool
anyway trips `PinnedBlockPool`'s domain-range assert rather than corrupting it.

**The stride is fixed by the first caller and asserted thereafter**, exactly as
radix's `ControlBlockStore` does it. This is a genuinely new hazard rather than
inherited caution: every old block was ≥ one page, so a block drawn when
`processorCount()` was smaller was still big enough; a 128 B-per-CPU stride
makes the same reuse an **out-of-bounds write into the next address space's
slots**. Unreachable in the kernel — the boot log confirms the kernel domain
already sees all 8 CPUs in `memory_management` — and immediately reachable in the
harness, which is where it fired.

### What this forced on the harness

`resetDomainManagementState()` was documented as mandatory for every fixture that
constructs a domain, and only `LifecycleTest` called it. That was harmless while
the shared state was a freelist — nothing reaches a freelist unless a test calls
`deinit` — and is not harmless now: the pool retains **carved blocks pointing
into the fixture's arena**, plus a stride, both of which outlive the munmap. Four
harnesses gained the call. The mismatched-stride assert is what surfaced it, at
18 failing tests; without it the same fixtures would have handed out dangling
memory whenever two consecutive fixtures happened to agree on CPU count.

### Mutation-tested

| Mutation | Caught by |
|---|---|
| pooled path disabled (`if (false && ...)`) | `..._pack_several_into_one_page` (block count) |
| stride halved, guard assert removed | `..._pack_several_into_one_page` (gap), `..._does_not_reach_into_its_neighbour` |
| multi-page arrays routed through the pool | `..._multi_page_slot_arrays_keep_their_dedicated_pages` |

Two of the checks were vacuous when first written and the mutations are what
showed it. The overlap loop sat *after* the block-count assertions, so the
stride mutation aborted the test before reaching it — whichever assertion runs
first is the only one a mutation demonstrates. And the neighbour test originally
checked only slot 7 of the neighbouring domain, while a half-stride lands the
corruption on a **different slot index** than the writer's; it now checks every
slot. Both were reordered/widened until the mutation killed the check it was
supposed to kill.

---

## D-049 — the freshness audit, walked deliberately

§7.1's freshness paragraph names four sites and two exemptions. §1.1 of the
handoff found **four more the hard way**, one per in-kernel stress boot, each
invisible to 150 userspace tests on two sanitizers. This is the deliberate walk
that should have preceded them: every class of memory the tree touches, checked
against the code and given a verdict rather than a defect report.

**Method.** A site is *covered* if the access goes through `SafePtr` (which
discharges per access, so no argument is required), *exempt* if the memory's
mapping cannot change under it, and a *gap* otherwise. Two exemptions rest on
arguments rather than on mechanism; both are listed with the fact they depend on,
because an argument in a comment is not a mechanism (D-044's own words).

### The classes

| Memory | Backing | Verdict |
|---|---|---|
| Node bodies — state word, refcount, slots | vmsmalloc | **Covered.** Every access is `NodeRef`, i.e. `SafePtr<void>::at<>`. There is no raw path: the only escapes (`raw()`) are identity, encoding, and the concrete-type cast `retire`/`destroy` need. |
| `Mapping` decoded from a slot word (lookup) | vmsmalloc | **Covered.** `SafePtr` at the decode site, and `LookupResult` *holds* it, so a result that crosses a pager round-trip and resumes elsewhere is covered too. |
| `Mapping` count word, deleter side | vmsmalloc | **Covered.** `releaseMappingRefs`/`acquireMappingRef` take `SafePtr` and every releaser has one. |
| **`Mapping` handed IN to `apply`** | vmsmalloc | **GAP — see below.** |
| **`Mapping` handed OUT by `enumerateChunk`** | vmsmalloc | **GAP — see below.** |
| `DeferredRelease` record body | vmsmalloc | **Covered**, first touch paid by `pool.pop()`; raw thereafter — *conditional exemption 1*. |
| Record's `next` written by `push` | vmsmalloc | **Covered** transitively: every caller has just made that record fresh (the deleter through `rec`, the draw through `pop`, creation through the allocator). |
| Root bucket page / bucket words | vmsmalloc, whole page | **Covered** per access through `buckets()`. This is exactly the cost **D-039** asks whether to keep paying — on the tree's hottest read. |
| `RetireHead` fields | vmsmalloc | **Exempt** — RCU's `onPreTouch` covers the head and only the head (RCU-DEC-006), which is why everything past it in this table is a live obligation. |
| Freshly allocated object, before publication | vmsmalloc | **Exempt** — *conditional exemption 2*. |
| Control block: generation, `dying`, pool heads | pinned | **Exempt** — DEC-082 round 4 put them there for this reason. Write-once PTEs never change, so no CPU's translation can go stale. |
| `ClusterTable` / `DeferredReleasePools` objects | pinned (control block) | **Exempt**, same reason. Only the *records* and the *bucket page* are vmsmalloc. |
| RCU slot block | pinned | **Exempt**, same reason (and now packed — D-048). |
| Descent-cache rows | `.bss` today | **Exempt by placement, not by construction — see the requirement below.** |
| Claim sets, attempts, cursors, segment plans | stack | **Exempt.** They hold node/`Mapping` pointers but dereference none. |

### GAP 1 — the `Mapping` handed IN to `apply`

`CoreTree::apply(lo, hi, Mapping* value, …)` takes a **raw pointer** and RMWs its
count word raw at three commit-phase sites (`WriteLeaf`, `OverwriteLeaf`, and
`DetachChild` when it writes a replacement): `value->acquireRef()`.

It works today for a reason nobody wrote down: the caller creates the `Mapping`
and applies it in the same syscall on the same CPU, so conditional exemption 2
covers it. That is an **unstated, unenforced precondition on a public API**, and
it is precisely the shape D-044 ruled on when the same argument was made about
`LookupResult` — "a freshness call made once at acquisition guarantees nothing to
a CPU that did not make it". Any caller that hands in a record created elsewhere
— fork-shaped work is the obvious candidate — gets a stale RMW on a count word,
which corrupts a refcount rather than faulting.

### GAP 2 — the `Mapping` handed OUT by `enumerateChunk`

`enumerateChunk` calls `fn(step.mapping, lo, hi)` with a **raw `Mapping*`**
decoded from a slot word, to an arbitrary consumer callback, with no freshness
obligation attached and nothing said about it in the API comment. Every real
consumer of an enumeration — `/proc/maps`, an `mprotect` range walk, a fork copy
loop — dereferences that pointer to read `baseVA`, `objectOffset` or the
protection, and it may be the first touch of that page on that CPU.

The lookup path had this exact defect and was fixed by making the *result carry*
the obligation. The enumeration path is the same shape and was missed — because
**the tests compare the emitted pointers by identity and never dereference them**
(`EnumerationTest.cpp`'s `collect` coalesces on `out.back().m == m`). Third time
the harness has been structurally incapable of seeing a member of this class.

**Both gaps are the same fix**: hand `SafePtr<Mapping>` across the boundary
instead of `Mapping*`, in and out. Mechanical — the encode path wants `address()`,
which discharges nothing and says so. It changes a **consumer-contract signature**
(§3), so it is Spencer's call rather than a silent repair.

### Conditional exemption 1 — records are raw after their first draw

`pop()` pays the first-touch call; every later access to that record (the dedup
walk, the field writes, the return-home walk) is raw on the same CPU. Legal
because a drawn record is owned, is never freed, and **nothing between the draw
and the last touch blocks or migrates** — the replenish that *does* block runs
between attempts, after `returnHeldRecords`. It depends on writers being
CPU-pinned. A scheduler that can migrate a writer mid-attempt invalidates it, and
this is the entry to revisit when one lands (DEC-030's predicates go live at the
same moment).

### Conditional exemption 2 — freshly allocated memory is fresh on the allocator

Raw writes immediately after `tryMake` (record initialisation, `BucketTable`
construction, `constructInto`'s placement-new itself) are covered because
vmsmalloc's allocation path already paid the call: it makes the slab
**descriptor** fresh before reading it, and slots live at `descriptor +
slot0Offset(c)` — the *same page*, so freshness for one is freshness for both.
The whole-page bypass is covered separately by DEC-046 (`allocPage` invalidates
this CPU's entry). It depends on slabs staying one page and the descriptor
staying on it. Worth knowing, because it is what makes every constructor in the
system legal.

### A requirement, not a site — where the descent cache lives

The cache's `rows[MAX_PROCESSOR_COUNT]` array is exempt only because it currently
sits in `.bss` (`RadixStress.cpp`'s `gCacheStorage`). Nothing enforces that. If
the eventual VMM allocates the cache from vmsmalloc, **every probe gains a
freshness call on the hottest path in the system** — the same trap DEC-082's
round-4 amendment avoided for the control block, one structure over. The cache
must live in `.bss` or pinned storage. Recorded here because it cannot be
`static_assert`ed.

### The testability finding

The harness's `ensureTLBEntryFresh` is `inline bool … { return false; }` — it
does not merely no-op, it **records nothing**, so no test can assert that a path
discharged its obligation. That is why every member of this class has been found
by an in-kernel stress boot, one at a time. Making the radix mock *count* calls
(and ideally the pages they name) would turn "reading an enumerated `Mapping`
discharges freshness for its page" into an ordinary assertion, and would have
caught both gaps above. Proposed, not done — it is a harness change with its own
design question (what granularity to assert at).

### Owed to the spec

§7.1's paragraph should carry the complete list: its four sites, §1.1's four, and
this entry's two, plus the two conditional exemptions with their dependencies
named. The reader-vs-deleter framing the paragraph implies is not the axis —
**the axis is whether the pointer crosses a boundary that a per-access mechanism
does not follow**, which is what all six unlisted sites have in common.

---

## D-050 — D-049's two gaps closed, and the harness taught to see the class

Spencer approved both signature changes; they landed with the instrumentation
that makes them assertable, which is the more durable half.

### The signatures

`Mapping*` became `VMSubstrate::SafePtr<Mapping>` at both boundaries the audit
found — in through `apply`/`runToCompletion`/`applyOrDecompose`/`decompose`/
`runAttempt`/`readPass`/`redispatch`/`commit` and `placeInCluster`, out through
`ScanStep::mapping` and therefore the `enumerateChunk` callback. Encode sites
take `.address()`, which discharges nothing and says so; that is the entire diff
besides the types.

The two directions behave differently at the call site, and the difference is
worth knowing:

- **Inbound is source-compatible**, because `SafePtr`'s converting constructor is
  not explicit — every existing caller still compiles. That is not a weakness:
  the caller's own pointer is fresh on its own CPU by construction, and what was
  broken was the *access*, which now discharges wherever the record came from.
- **Outbound is a hard break**, and deliberately: a callback taking `Mapping*`
  no longer compiles, so the four in the suite had to be looked at. That is the
  forcing function the raw signature never provided.

`guardIsClear`'s callback is the one shape that owes nothing — it never names the
record, because occupancy is the whole question.

### The instrumentation, which is the part that lasts

`tests/kernel/radix/mocks/MockFreshness.cpp`. The mock's `ensureTLBEntryFresh`
now records, per thread, which pages it was called for; `VMSubstrate::test`
exposes `armFreshnessRecording`, `clearFreshnessRecord`, `freshnessCalls`,
`pageWasMadeFresh` and `freshnessRecordOverflowed`. `FreshnessTest.cpp` (six
tests) asserts against it.

Three decisions:

- **Per thread, not global.** Freshness is a property of one CPU's mapping —
  D-044's whole content — so "this thread discharged freshness for this page" is
  the proposition. A global record answers the weaker "somebody did", which is
  precisely the weaker claim that let a `Mapping` acquired on one CPU be read on
  another. `radix_freshness_record_is_per_thread` pins that down.
- **Armed explicitly.** Disarmed it is one thread-local bool test per access, so
  the concurrent, torture and soak runs are unperturbed and nobody has to wonder
  whether the instrumentation moved a race.
- **A fixed array, not a set.** No allocation on the recording path, so the
  harness's per-test byte accounting keeps meaning what it says. The bound is
  reported rather than hidden — `freshnessRecordOverflowed()` exists so a test
  cannot pass on a truncated record, the only way this could lie in the direction
  that matters.

**What it still cannot see: whether a call was NEEDED.** No page tables, so a
superfluous call is invisible and stays a question for `-icount` and D-042. The
tests are one-directional by construction, catching the absent call — the
direction that corrupts memory.

### Two tests that had to be rewritten before they meant anything

- The descent test first asserted a **call count** (`>= 3`) and passed at 2 for a
  reason that had nothing to do with the discipline: the fixture was not deep.
  Nor was the fix "use a bigger span" — a single record over two pages is stored
  as ONE leaf carrying a sub-range, so a wide root slot holds it without
  subdividing at all. It takes **two distinct records** to force a subdivision.
  It is now stated as a comparison — a deeper descent must discharge *more* than
  a shallow one — because the absolute count is an implementation detail and the
  per-level property is not. Counting distinct *pages* would not work either:
  two nodes of one size class can share a slab page.
- The first test asserts what `address()` must NOT do. If it ever starts
  recording, every other assertion in the file goes vacuous.

### Mutation-tested

| Mutation | Caught by |
|---|---|
| `value.address()->acquireRef()` — the pre-fix raw RMW | `..._apply_discharges_freshness_on_the_incoming_record` |
| consumers read through `.address()->` — the pre-fix world | `..._lookup_result_...`, `..._enumerate_hands_out_a_pointer_...` |
| `NodeRef::slot` reads through `address()` instead of `at<>` | `..._descent_pays_the_discipline_at_every_level` |

Suite 163×2 (was 156), kernel clean on all three configs, debug and Release/LTO.

---

## D-051 — the pinned allocator became a template over its backend, and the root bucket page moved into it (Spencer-directed)

Spencer's framing, and it settles **D-039**: a second allocator for the root
bucket page, alongside the control-block one, both backed by pinned memory — and
the allocator itself templated so the *backend* can later become something that
reclaims virtual addresses lazily (a VA cannot be handed out again until every
CPU has flushed its entry for it), with NUMA-awareness built in from the start.

### One collapse, recorded because it is the design

**The template parameter is the BACKEND, not the consumer.** There are three
pool instances in the kernel now — radix control blocks, radix root pages, RCU
slot blocks — and they differ only in stride, which has to stay a runtime value
since two of the three derive it from `arch::processorCount()`. Templating over
the consumer would buy nothing; they are three `init()` calls on three objects.
NUMA likewise stays in the *layer*, where it already was: per-domain freelists,
preferred-domain allocation, carve-before-cross-domain-fallback, and a block that
reports where it actually landed. A backend only has to answer "give me pages on
domain D".

### The trait is the seam's real payload

Backends differ in something more important than where pages come from: **whether
a page's mapping can change under a consumer**. `PinnedBackend`'s cannot
(DEC-051b: not-present -> present exactly once, never changes), which is the
whole reason pinned storage carries no freshness obligation. A VA-recycling
backend's does, and every block it hands out re-enters the `SafePtr` discipline —
the class this subsystem has now shipped eight instances of.

So `BlockPool` exports `blocksAreImmutablyMapped`, and a consumer that reads its
blocks raw is expected to `static_assert` on it. `ClusterTable::buckets` does.
DEC-082's argument — "the generation check, the `dying` load and the pool heads
carry no freshness obligation at all" — stops being a paragraph and becomes a
compile error if a backend is ever swapped under a consumer that is not ready.

### What the seam does NOT buy, stated before it is discovered

Swapping the backend is necessary but not sufficient for whole-page reclaim: to
return a page, something must know no live block remains on it, and a flat
per-domain freelist knows *blocks*, not *pages*. A reclaiming backend also needs
the LAYER to grow per-page occupancy plus a return policy (immediately at zero,
or with hysteresis — alternating alloc/free at a page boundary would otherwise
thrash the quarantine). Deliberately not built: no consumer, and the policy is a
real design question rather than a mechanical one. The seam is still useful
without it, since a reclaiming backend can enforce deferred reuse on its own side
from day one.

### The root page: a second pool, not a bigger stride

| | root page in its own pool | folded into the control block |
|---|---|---|
| stride | 4,096 B — exactly one page | 4,864 B — *larger* than a page |
| packing | perfect | impossible; 41% lost to page rounding |
| window per address space | **4,096 B** | 8,192 B |
| total per address space | **5,939 B** | 9,216 B |
| ceiling | **~180,000** | ~116,000 |

A second instance is worth 2x here, which is why the fold was the wrong shape for
the same idea. ~180,000 concurrent address spaces against a consumer-desktop
target is not a constraint.

What it buys is not packing but the **mapping**: `ClusterTable::buckets` and
`CoreTree::buckets` return raw pointers again, so **every descent's opening
bucket read no longer discharges a freshness call** — the hottest read in the
tree. It also takes a whole-page vmsmalloc alloc/free out of every address-space
lifecycle, which is the exact churn that produced D-039's stale-read bug.

### Lock discipline, which is where this could have gone wrong

The pool's contract is that the caller holds the domain-management lock, and
`Domain::init`/`deinit` take that lock *themselves* (RCU-DEC-043). So the root
page's draw sits in its **own** scope after step 2 rather than sharing step 1's
hold, and its return sits in its own scope before `deinit` rather than sharing
step 8's. Both are one-line scopes whose absence would be a self-deadlock, so
both say why.

### Verified

Suite 164x2 (one new test). `radix_root_bucket_page_is_exempt` asserts the
negative against a live positive in the same recording window — the record's page
made fresh, the bucket page not — because a bare negative would pass just as well
with the instrumentation switched off. **Mutation-tested**: restoring
`CoreTree::buckets`'s `SafePtr` fails it. Kernel clean on run/run_numa/
run_numa_hmat, debug and Release/LTO; debug 2,049 cycles, Release 8,193.

### Still open

**D-042 is not settled by this.** Nodes cannot be pinned — their count per
address space is unbounded — so per-level descent freshness stands until either a
reclaiming backend can back them (this design's roadmap, and D-042's option 2) or
vmsmalloc's guarantee is strengthened (option 3).

---

## D-052 — D-042's multiplier, measured (and D-042's own estimate vindicated on the read path)

D-042 prices per-level node freshness as *"one `ensureTLBEntryFresh` per level on
the descent"*. That number dated from when the call sat where a node pointer was
DERIVED — seventeen sites, one per node per walk — and D-044 then moved it to per
ACCESS, which is strictly more calls. Nothing re-measured it. The D-050
instrumentation makes it a two-minute question, so:

```
  freshness calls per LOOKUP, by tree depth (two adjacent single-page records)
      root level | depth | total | on nodes | on the record | distinct pages | per level
               6 |     1 |     2 |        1 |             1 |              2 |      1.00
               5 |     2 |     3 |        2 |             1 |              2 |      1.00
               4 |     3 |     4 |        3 |             1 |              3 |      1.00
               3 |     4 |     5 |        4 |             1 |              3 |      1.00
               2 |     5 |     6 |        5 |             1 |              3 |      1.00
               1 |     6 |     7 |        6 |             1 |              3 |      1.00

  freshness calls per APPLY (overwrite of one page), by tree depth
      root level | depth | total | distinct pages | per level
               6 |     1 |    10 |              2 |     10.00
               5 |     2 |    22 |              2 |     11.00
               4 |     3 |    27 |              3 |      9.00
               3 |     4 |    32 |              3 |      8.00
               2 |     5 |    37 |              3 |      7.40
               1 |     6 |    42 |              3 |      7.00
```

### What it says

- **The read path is exactly 1.00 calls per level, plus one for the record.**
  D-042's estimate survives the move to per-access unchanged, because a lookup
  touches exactly one field per node — the slot it descends through. The state
  word and refcount are write-path fields. The worry that D-044 had silently
  inflated the fault path was wrong, and worth writing down as wrong.
- **The write path pays 7–11 per level** — an order of magnitude more, because
  acquisition, the claim, re-dispatch and commit each re-read through the node.
  That is `mmap`/`munmap`, which already allocates and locks; it is not the path
  D-042's cost argument is about, but it is where per-access actually bit.
- **The calls concentrate on 2–3 distinct pages even at depth 6**, because
  same-size-class nodes share a slab page. `ensureTLBEntryFresh` checks a per-CPU
  dirty-bitmap word for the page and `invlpg`s only if set, so **only the first
  call per page can miss**; the rest hit a line already in L1. This is the
  measurement that most weakens D-042's cache-miss worry.

### What it does NOT say, and the distinction is load-bearing

**The call COUNT is structural** — it falls out of how many fields a walk reads,
so the harness measures it exactly and the number transfers to the kernel.

**The page SPREAD is allocator state**, and the harness's is a best case: a fresh
mmap'd arena packs a descent's nodes into 2 pages. A long-running kernel with a
fragmented arena will spread the same descent across more, and every additional
page is another candidate first-touch miss. So "2–3 pages" is a floor, not an
estimate, and the cache-miss question D-042 asks still needs real silicon or a
model — which is exactly why the entry asked for a cache-miss measurement rather
than `-icount`.

### Owed to D-042

Option 1 (keep and price) is cheaper than the entry implies for the path that
matters: one call per level descended, on a walk the descent cache already
short-circuits ~97% of the time (DEC-079's calibration report). The remaining
input is what one call costs when it misses, which `-icount` cannot answer and
this harness cannot either.

---

## D-053 — D-042's per-call cost, measured on target under `-icount`

D-052 measured the descent's freshness call COUNT in the harness and could say
nothing about what a call costs, having no page tables. This is the other half,
on the real toolchain: RCU P4-ITEM-002's probe machinery applied one subsystem
over, deliberately the same shape so the two subsystems' figures are comparable.

`CROCOS_RADIX_INSN_PROBE` = 1 (bracket one freshness call), 2 (bracket a whole
`lookup`), 3 (COUNT the calls one lookup makes). Never combined — mode 1's probes
inside a mode-2 bracket would inflate the denominator by their own cost, and mode
3's counter perturbs both. `run_icount` is the new target: `-icount shift=0` with
`-accel tcg,thread=single` (icount and MTTCG are mutually exclusive) and `-smp 2`,
since deterministic single-threaded TCG replays every vCPU on one host thread and
the probes want samples rather than concurrency.

Release/LTO, minima over the sample counts shown:

| Measurement | Ticks | Net of probe | Samples |
|---|---|---|---|
| empty probe (baseline) | 3 | — | — |
| **one freshness call, hit path** | 27 | **24** | 293,368 |
| **one `lookup`** (min / mean) | 519 / 800 | **516 / 797** | 521,709 |
| **calls per `lookup`** (min / mean) | 1 / 3 | — | 391,056 |

### What it comes to

**The freshness calls are ~10–20% of a lookup's instructions**, and ≤23% even
if every lookup paid a full cache-missing descent (5 calls: 4 levels from the
default root level 3, plus the record). The range is honest rather than coy: the
mean lookup ticks are interrupt-contaminated upward while the mean call count is
not, so pairing mean-with-mean understates and min-with-mean overstates.

For scale, RCU P4-ITEM-002 measured the same call at **51–59% of a retire**. The
descent is not freshness-dominated the way the retire path is — it does far more
other work per call.

### Two things the numbers say that the setup nearly hid

- **24 instructions, not the ~40 D-042 quotes.** That figure was pre-LTO; this is
  a Release/LTO build, and the same LTO effect P4-ITEM-007 measured on the retire
  path applies here.
- **The `-icount` config reports a 74% descent-cache hit rate against MTTCG's
  7%**, because single-threaded TCG gives each vCPU long uninterrupted bursts and
  therefore far better locality than real interleaving would. So the measured 3
  calls per lookup is a LOW estimate of what an 8-CPU machine does; the ceiling
  is the full-descent 5. Both bracket ends are given above rather than a single
  number that would quietly depend on the harness's scheduling.

### What is still not measured, and cannot be here

The **cost of a call that misses**. `ensureTLBEntryFresh` loads a per-CPU
dirty-bitmap word and `invlpg`s only if the bit is set; TCG models neither the
cache miss on that word nor `invlpg`'s pipeline cost. Every figure above is an
instruction count on the hit path. D-052's finding that a descent's calls
concentrate on 2–3 pages bounds how many misses are even possible, but the price
of one remains a question for real silicon — exactly as D-042 said when it asked
for a cache-miss measurement rather than `-icount`.

---

## D-054 — ITEM-084's evidence: the draw-count distribution, and why the workload could not produce it

ITEM-084 asks whether DEC-068's eager sizing is right — every (CPU × address
space) pool at the per-operation ceiling, **230 records, ≈14.4 KiB realised per
CPU per address space** — or whether a smaller reserve behind the abandon-and-
`barrier` replenish would do. The item says the answer "cannot be answered from
first principles — it needs the draw and shortfall distribution from a realistic
workload". So: a histogram, sampled at the commit boundary (the peak an attempt
actually held, since abandonment returns records).

### The realistic workload

Release/LTO, 8 CPUs, **1,912,936 committing attempts**:

```
n=1912936 max=2 ceiling=230
  0:1.42M  1:0.49M  2:9  3:0  4:0  5-8:0  9-16:0  17-32:0
  33-64:0  65-128:0  129-230:0  OVER-CEILING:0
```

**Max 2. The pool is provisioned at 115× the observed peak, and 0 shortfalls.**

### Why that number is nearly worthless on its own, and what fixed it

The first run of this histogram was a trap of exactly the kind §1.1 keeps
producing: **the stress cannot generate the expensive shape.** Every unmap row
cleared precisely the span it had placed — a FULL cover of one record — and §7.1
is explicit that the full cover is the *cheap* row (the subtree detaches and
every release rides a node deleter, drawing nothing). The expensive shape is a
**partial** cover displacing many distinct records at once. A histogram over a
workload that structurally cannot produce it says "this workload is cheap", not
"the ceiling is over-provisioned", and reporting the first as the second would
have been the whole point of the measurement thrown away.

Two things were added rather than one, because either alone is misleading:

1. **A bulk-unmap row in the stress** — a range covering whatever mappings lie in
   it, which is also the more realistic `munmap`: a process tearing down a region
   unmaps a RANGE, not one mapping at a time. 124,823 of them in the run above.
2. **Constructed shapes in the harness**, since even with bulk unmaps the max
   stayed at 2 (see below). These build the shape on purpose:

```
  DeferredRelease draws for a partial cover of one node (ceiling 230)
      distinct records in the node | cleared | drawn
                                 2 |       1 |     1
                                 4 |       3 |     3
                                 8 |       7 |     7
                                16 |      15 |    15
```

**One record per distinct displaced mapping, exactly as §7.1 models it** — and
the same node unmapped one slot wider draws **zero**, because the full cover
detaches. §7.1's inversion is now tested rather than asserted, which it never was.

### Why the workload still does not reach it, which is the actual finding

Even with bulk unmaps the max stayed at 2, and the reason is not that the shape
is unreachable but that **the placement policy actively avoids it**: DEC-032
probes with random guard gaps precisely so mappings are not adjacent, so a node
holding many DISTINCT records is rare by construction. The edge sum assumes a
fully-populated tree with a distinct record in every slot — the arrangement the
allocator is designed not to produce.

That reframes ITEM-084. The question is not "is 230 too big for the average
case" (it plainly is, by 115×) but **"what produces the arrangement the ceiling
assumes, and does anything realistic do it?"** Candidates the current stress does
not model: `MAP_FIXED` packing by a userspace allocator that manages its own
adjacency, and `fork`, whose per-mapping copy is inherently dense.

### The constraint any answer has to respect

Shrinking the reserve is not simply a memory/latency trade. §7.1's replenish
terminates because the pool's population is **at least one operation's worst
case**: a draw short of that abandons, `barrier`s (which brings every record
home), and retries — and the retry succeeds *because* the full population
suffices. A pool smaller than the operation needs turns that into a livelock, not
a slow path.

So the live options are:

1. **Keep eager 230.** Costs ≈14.4 KiB per CPU per address space (≈115 KiB per
   process on an 8-CPU desktop, per [[project_crocos_target_hardware]]).
2. **Shrink to a measured per-attempt bound**, only if the true per-attempt
   maximum is provably smaller than the edge sum — which is a question about
   DEC-077's unit decomposition, not about this histogram: **if one attempt can
   never span the whole edge sum because §6.5 decomposes first, the bound is
   already smaller than 230 and nobody has computed it.** That is the open
   question this measurement surfaces and does not answer.
3. **A global emergency reserve** under the domain-management lock for the rare
   operation that exceeds a small per-CPU pool, restoring termination without the
   per-CPU cost. Not proposed anywhere yet; recorded so the option is not lost.

**Not decided.** The distribution is now measurable on demand
(`CROCOS_RADIX_DRAW_HISTOGRAM`, implied by the stress) and the worst case is
reproducible in the harness, which is what the item asked for.
