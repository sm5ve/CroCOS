# radix-tree implementation — deviations and spec findings

Running log for the implementation branch (`radix-tree`), kept per
[[feedback_spec_deviations]].

**Starting a fresh context?** Read `docs/radix-tree-phase-3-HANDOFF.md` first — it
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

## D-032 — GAP (2026-08-08, Phase 3) — the teardown walk is not yet DEC-100's

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
