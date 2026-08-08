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
