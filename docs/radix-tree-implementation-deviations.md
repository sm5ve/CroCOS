# radix-tree implementation — deviations and spec findings

Running log for the implementation branch (`radix-tree`), kept per
[[feedback_spec_deviations]]. The user is asleep and delegated
document-and-proceed for anything short of a fundamental error, so every
judgement call made without sign-off is recorded here with its spec citation and
the reasoning, for review in the morning.

Categories:

- **FINDING** — the spec is wrong or internally inconsistent. Nothing was
  changed in the spec; the implementation follows the reading given.
- **CHOICE** — the spec is silent or underdetermined and the implementation had
  to pick. States what was picked and why.
- **GAP** — something the spec requires that is deliberately *not* implemented
  yet, with the phase it belongs to.

---

## D-001 — FINDING — vmsmalloc DEC-049's slots-per-slab figure is off by one at 192 B

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
