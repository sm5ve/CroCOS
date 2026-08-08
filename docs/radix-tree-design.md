# The CroCOS Radix Tree — Design Document

*Distilled 2026-08-08 from `specs/radix-tree.md` (101 decisions, `status: review`) for design
review. This document is narrative and explanatory; the spec is normative. `DEC-nnn` and
`ITEM-nnn` references point into the spec's decision record and open-questions table. Where this
document and the spec disagree, the spec wins and this document has a bug.*

---

## 1. What this is and why it exists

The tree is the authoritative *software* partition of a virtual address space: a concurrent,
RCU-protected prefix tree mapping virtual address ranges to `Mapping` records, giving lock-free
lookups on the page-fault path and mutation that does not serialize across unrelated address
ranges. It is RadixVM-derived, not a RadixVM implementation.

The motivating question is what OS design decisions become available when `mmap`/`munmap` are
cheap. This is not speculative optimization: the cost of a primitive gets encoded into its
callers as workarounds, and the workarounds are invisible afterward. The canonical precedent is
Mach and early L4 building IPC on page remapping, then discovering that copying beat mapping for
small messages — a result that shaped a generation of microkernel IPC design and was contingent
on VM operations being expensive. Since IPC is CroCOS's other core primitive, that calculus is
directly at stake. (Note "cheap `mmap`" is mostly "cheap TLB shootdown" — local unmapping was
never the expensive part — which is why shootdown minimization is a first-class design input.)

Two things are worth keeping from the RadixVM paper:

1. **A prefix tree's shape is determined by the key, not by insertion order**, so it never
   rebalances. A red-black tree or B-tree mutates shared upper nodes on rotation — contention
   that exists purely as an artifact of the data structure. In a prefix tree, disjoint keys touch
   disjoint memory as a structural property, not a tuning outcome (DEC-006).
2. **Shootdown minimization** — tracking which cores actually hold a mapping so unmapping
   interrupts only those cores, often none.

Two departures dominate everything below:

- **Leaves live at any level** and name a VMObject mapping, rather than sitting at a fixed
  per-page leaf level (DEC-003). The paper pays per-page slots for its headline
  disjointness result; we decline that memory cost and accept a conditional version of the
  result (§5.2 below).
- **The root is a one-page prefix index over the whole address space** whose entries point at
  *clusters* — subtrees rooted at whatever level suits their size — so depth is paid only where
  used (DEC-009/027/030/033).

A cautionary note that shaped the record: the mechanism we all "remembered" from sv6 — a lock
bit in the pointer word — turned out, on reading the actual source, to belong to a dead
implementation its authors deleted ("ancient, unused"). The real RadixVM artifact
(`radix_array.hh`) obliges the *value type* to donate a lock bit and rounds range locks
*outward* to folded-run boundaries. Both facts favor the design below: our claim state lives in
a separate per-node word, so slot words stay pure codec, and our bit-granular claims never
over-lock (DEC-086).

---

## 2. Geometry

### 2.1 Buckets and clusters

The root is **one 4 KiB page of 512 buckets**, indexed by the top 9 bits of the VA. Buckets tile
the 47-bit user space at 256 GiB each and hold **at most one cluster apiece** — the index is a
prefix, not a hash: no probing, no collisions (DEC-030/033).

A **cluster** is a node of one conceptual full-depth tree, rooted at whatever level suits its
current size. Its base is span-aligned by construction, and it grows by allocating a node
*above* itself: the old root becomes one child slot of the new node, so no mapping moves. Each
bucket entry is a pointer to an immutable **cluster descriptor** — base VA, level, root pointer —
and growth mints a fresh descriptor and publishes it with a compare-exchange, retiring the old
one (DEC-027/028).

Why clusters at all: rooting one tree over the full 47-bit VA forces full depth immediately;
rooting lazily over only the span in use makes the tree shallow but collapses ASLR entropy.
Clusters give both — compact shallow subtrees with bases scattered across the space. Over 47
bits at 64 KiB placement granularity there are 31 bits of placement entropy and clustering
preserves all 31 (9 bucket + 8 base-within-bucket + 14 within-cluster). What it costs is
*conditional* entropy: an attacker who leaks one address learns its cluster base, leaving 14
bits for other mappings in that cluster at the 1 GiB default. Linux has the same weakness via
`mmap_base`.

### 2.2 The bit split

`9 / 4 / 4 / 5 / 5 / 4 / 4 = 35 = 47 − 12`, spanning user VA to the 4 KiB floor with no slack.
Italicised levels exist only for grown or large-rooted clusters:

| Level | Bits | Slots | Span/slot | Node span | Structural | Realised |
|---|---|---|---|---|---|---|
| root bucket | 9 | 512 | 256 GiB | 128 TiB | 4096 B | 4096 B |
| *G1* | 4 | 16 | 16 GiB | 256 GiB | 160 B | 192 B |
| *G0* | 4 | 16 | 1 GiB | 16 GiB | 160 B | 192 B |
| **C0** (default root) | 5 | 32 | 32 MiB | 1 GiB | 288 B | 320 B |
| C1 | 5 | 32 | 1 MiB | 32 MiB | 288 B | 320 B |
| C2 | 4 | 16 | 64 KiB | 1 MiB | 160 B | 192 B |
| C3 | 4 | 16 | 4 KiB | 64 KiB | 160 B | 192 B |

In a default C0-rooted cluster, a 64 KiB-granular mapping terminates as a leaf in a C2 node:
**four steps including the root**, matching the hardware page walk; page resolution costs a
fifth, paid only where used.

**The geometry is a `constexpr` descriptor the tree is templated on, with one kernel-default
instance per architecture** (DEC-019, sharpened by user direction in DEC-093): retuning the
split — or shipping a different split on arm64 — must cost an edit to one descriptor plus
re-derived figures and *nothing else*. This is doubly load-bearing: it is what lets the test
harness instantiate a deliberately tiny geometry for exhaustive model checking, and the harness
constraint (nothing may special-case the shipped numbers) is the same constraint that keeps
retuning cheap. A 2 MiB-congruent alternative split (9/4/4/4/5/5/4, making C1 slots exactly
2 MiB) is catalogued in DEC-093 and is one descriptor edit away; it was declined because
leaf-span congruence with huge pages buys nothing mechanical — the page-table manager chooses
page sizes under a mapping independently of which leaves name it — and, interestingly, its
originally-recorded cost (a worse shortfall column) turned out on re-derivation to be zero.

**Why valence caps at 32** (DEC-039): at 32 slots the claim bitmap, occupancy count and dying
mark fit one 64-bit word, so every protocol primitive is a single atomic on one location. At 64
the bitmap alone fills the word, the count moves to a second word, and the check-then-act race
between insertion and reclamation returns. The size-class result (288 B structural fits the
320 B class) is a consequence, not the reason.

### 2.3 Realised sizes

`vmsmalloc` gained size classes {192, 320} with a **contractual 64 B alignment** for this
consumer (vmsmalloc DEC-049, radix DEC-076) — 1.11–1.2× structural, down from the 1.6–1.78× the
old {256, 512} realisation cost. The headroom above 288 B structural is exactly the 64 B-aligned
state word ITEM-055 may want; past 320 B the next class is 512 B, then a whole page. Every
memory figure in the spec uses realised sizes: a saturated 1 GiB cluster is ≈202 KiB, ≈3.2 MiB
if pinned at page granularity by pathological `mprotect` churn.

---

## 3. Representation

### 3.1 The slot word

A slot is one 64-bit word, exactly one of three things (DEC-021): all-zero = empty; bit 0 set =
encoded leaf (`Mapping*` + covered sub-range); bit 0 clear and non-zero = encoded child pointer.
Child words set **bit 1 (`kChildGuardBit`)**, masked on decode, so a child pointer is
structurally non-zero at any allocation offset — without it, a node allocated at `VMSubstrate`
offset 0 under the compressed codec would encode to all-zero and alias the empty slot, a defect
the userspace harness structurally cannot reach because its mock arena lives in the low half
(DEC-081). Leaf bits 1–3 stay reserved: duplicating protection bits there was declined because
both fault paths dereference the `Mapping` anyway and `mprotect` would have to rewrite every
leaf slot in range (DEC-089).

What the remaining bits mean is a **codec's** business (DEC-023): the core tree is templated on
a codec that round-trips a value through one word and optionally supplies a `covers` predicate.
Two instances exist — uncompressed for the userspace harness, compressed for the kernel. The
compressed codec's budget is 25 upper bits, fixed by the deliberate decision *not* to confine
node allocation to a VA sub-window (DEC-024/061): confinement would widen the budget to 38 bits
but would cap node memory globally rather than per NUMA domain — a scaling limit on precisely
the axis a NUMA machine must scale. The budget is nearly spent (one spare bit at C0), and the
obvious relief under future bit pressure is exactly the confinement this rule forbids — stated
loudly so nobody trades it away by accident.

**Sub-range leaves** (DEC-022/025): a leaf may cover a contiguous sub-range of its slot's span,
encoded inclusive-end in the spare bits at the finest power of two the budget allows per level
(8 KiB at C0, the 4 KiB floor at C1 and below). This is what keeps page-granular partial
`munmap` and `mprotect` — which POSIX forces and the 64 KiB placement granularity cannot
mitigate — from building tree depth: an edge shrink is a single in-place word rewrite with no
allocation and no count movement. The encoding is **never load-bearing**: any range not
expressible at a level's granularity subdivides one level instead, so retuning granularity
changes node counts, never semantics.

### 3.2 The node state word

Every non-root node carries one 64-bit atomic word; **field order is a correctness decision**
(DEC-041):

| Bits | Field | Mutated by |
|---|---|---|
| 0–31 | claim bitmap | `fetch_or` acquire / `fetch_and` release |
| 32 | dying mark | unconditional `fetch_or`, only under a whole-node claim |
| 33–38 | occupancy count | `fetch_add`/`fetch_sub` release |
| 39–63 | spare, held at zero | — |

The mark sits *below* the count so that count arithmetic (`±(1 << 33)`) can never carry or
borrow into it — and padding above the count would not help, because a borrow propagates upward
*through* zeros to the nearest set bit: with the mark above a zero count, one spurious decrement
clears the mark, a use-after-free from an accounting slip two mechanisms away. Detection is
asymmetric: underflow sets spare bits at the first slip; overflow passes silently for 31–47
excess increments, so the debug range assert is the only detector and there is none in release.

A node also carries a refcount word (unconditional on every node — the realised classes make it
free, and opting out would fork the layout for zero bytes, DEC-078) and a 16 B inline
`RetireHead`, for 32 B of header. Node metadata is **write-path-only** (DEC-012): descent needs
the slot array and the discriminant, nothing else — level is known by counting from the
descriptor, a subtree's base by masking the key — so writers use a descent stack rather than
parent pointers, and metadata placement is free of hot-path considerations (ITEM-055 keeps the
exact placement open pending measurement).

### 3.3 The `Mapping` record

A leaf names an interposed record — `VMObject*`, **object offset**, base VA, protection, and a
**max-protection ceiling** — rather than inlining any of it in the slot (DEC-014/087/088). The
object offset resolved at a fault is `objectOffset + (faultVA − baseVA)`, and both terms matter:
without the record's own offset, `mmap(fd, offset ≠ 0)` is unrepresentable and DEC-080's
per-bucket fragments would all alias the object's first frames; anchoring the subtraction at the
leaf's slot base instead of the mapping's base resolves every address in a leaf to its first
frame. Either slip is a silent wrong-frame bug invisible to the partition invariant. (The
missing offset field survived five review waves before a referee caught it — the record's field
list is worth a careful look in review.)

Protection lives on the mapping, not the object — *objects differ when the bytes can differ;
mappings differ when the access differs* (DEC-087; prior art is unanimous, and per-object
protection would force `mprotect` to split objects, reinventing the mapping layer inside the
object layer). The max-protection ceiling is fixed at creation from whatever conveyed the right
to map and is never raised (DEC-088) — capability-shaped, cheap now and expensive to retrofit.

One record is named by many leaf slots, which makes its reference counting an N:1 rule (§6.3).

---

## 4. The concurrency protocol

### 4.1 Shape of an operation

Every mutation is: **read-only descent → allocate → acquire → re-dispatch → | → mark →
reference → publish → retire → release**, with the bar the commit boundary (DEC-043/044).
Writers are CPU-pinned with preemption disabled and run the whole attempt inside one RCU read
section — not as a formality: a writer traverses the shared structure, so an unpinned one can
have a node reclaimed underneath it. One attempt is one section; closing the section ends the
attempt and discards the claim set. Every attempt (except teardown's own units) begins by
checking the address space's `dying` flag.

The read-only pass is separate because a writer cannot know it needs a *parent's* claim bit
until it has read the *child's* occupancy — discovering the set while acquiring would acquire
out of order, and claiming every descended-through slot would make disjoint operations conflict
at every shared ancestor. Nothing irreversible precedes the boundary (a mark set by an operation
that then backs off leaves *no symptom* until something tries to map that range, forever);
nothing after it can fail.

Writers reserve slots by setting bits in the owning node's **claim bitmap** (DEC-036).
`fetch_or` cannot be conditional, so claiming is speculate-then-validate: OR the mask, inspect
the prior, and touch no slot unless the mark and every masked bit were clear; otherwise clear
`mask & ~prior` — never `mask`, which would clear the winner's bits — and retry. **One
`fetch_or` per node per acquisition, never revalidated** (DEC-046): from the instant a writer
validly holds a bit, no actor can mark that node (all three mark paths go through a whole-node
`fetch_or`, which fails against any held bit), and the validation primitive is not idempotent —
a second issue returns the writer's own bits, reads as a loss, and self-livelocks one CPU with
no contention.

Bit granularity is load-bearing, not an optimisation: two disjoint contiguous ranges conflict at
exactly one node — the deepest where both actually write — where a word-level CAS would
serialize them at every shared node and fail as a mysterious contention result. The one place
the design is strictly weaker than the paper: two operations that both force expansion of the
*same* leaf contend transiently on its parent slot, once per leaf lifetime. This conditional
disjointness is accepted as the requirement (DEC-085), with a conflict counter as the standing
regression detector, because the paper buys the unconditional property with per-page slots —
the memory cost DEC-003 exists to refuse.

### 4.2 Publishing and dispatch

A publish is a **release-store** into a claim-protected slot, not a CAS (DEC-055): every slot
writer holds the bit, so the writer is exclusive and arbitration has moved to the claim. A write
dispatches on the slot's current content — eight exhaustive rows: empty (write), covered leaf
(overwrite), partial leaf with two survivors or an inexpressible boundary (build a child
subtree — a **middle punch** through a coarse leaf builds a spine, ~45 leaves for 4 KiB through
32 MiB, not two; DEC-066), partial child (descend, no claim here), fully-covered child (detach,
budget permitting), partial leaf with one expressible survivor (in-place shrink — no allocation,
no count movement), occupied-being-cleared (store zero, then count decrement), empty-being-
cleared (no-op — a spurious decrement is the borrow the state-word layout exists to catch).

The one publish site outside the claim protocol is the **root-bucket word**: it has no state
word, so growth's descriptor CAS *is* the arbitration, release on success, with every bucket
read through `kernel::rcu::protect` (DEC-028/069). A losing grower shallow-discards and retries;
a losing *creator* adopts the winner's cluster — and creation deliberately publishes an **empty**
cluster and places into it afterward, because a pre-populated private root would carry `Mapping`
references through a losable CAS with no safe revert (DEC-067).

### 4.3 Reclamation

Empty-node reclamation is mandatory (DEC-017): random-probe placement means a freed region is
essentially never reused, so nothing would ever reclaim by reuse. The subtlety is that a node's
emptiness depends on a *descendant's* reclamation succeeding, and the read pass holds no claims,
so candidacy from the pass is advisory. The decision is computed **at the commit boundary, from
the priors the claims froze** (DEC-064): each candidate's exact count came from its own
`fetch_or` prior, the whole-node claims make every count immune to change, and "which candidates
actually empty" becomes a pure bottom-up computation over data the writer already holds, run
before the first mark. Treating *every* node on the path as a candidate would be wrong and
costly — whole-node claims at every level destroy the disjointness the design exists for.

The walk terminates at the cluster root, which is never a reclamation candidate: its parent is a
bucket word with no interlock, and reclaiming it would leave a descriptor pointing at freed
memory — an entire 256 GiB bucket permanently unmappable with no crash.

### 4.4 Detachment and the budget

Detaching a fully-covered subtree is **claim-then-mark**, in two non-optional phases: phase one
whole-node-claims top-down (reversible, may fail and release), then a **freeze-and-verify
re-walk** under the claims — a concurrent probed placement can legally build nodes into a hole
of the subtree between the unclaimed count and the claims landing, and without the verify the
marks would orphan the newcomers unmarked and live — then phase two marks, once nothing can
fail. Every node of the subtree is marked, not just the root: the descent cache holds *interior*
nodes, and an unmarked interior node inside a detached subtree reads as fresh and silently
resumes a descent into a remapped range.

**Detachment is budget-bounded, cumulative per attempt** (DEC-077, `detachBudget` provisionally
64). The unbounded form fails on three axes at once: the failure-probability × wasted-work
product grows with the subtree on both factors; the span is non-preemptible; and the whole span
sits inside one read section, stalling EBR reclamation domain-wide — reachable by an ordinary
`MAP_FIXED`. Over budget, the operation decomposes per child slot at the **deepest node
containing its entire range**, each intersected slot its own bounded unit (partial leaves take
the edge rows, partial children recurse with their sub-range as a sub-operation, empty slots are
written for a map and no-ops for a clear), with the coarse-value replacement of the flattened
site only when a value-writing operation fully covers a non-cluster-root site. Hole-freedom is
untouched — every address still transitions old-to-new at exactly one store — and the claim set
is a fixed-capacity `detachBudget + siteBound` array, never a heap allocation.

Review note: this decomposition paragraph (spec §6.5) went through **seven rewrites** across the
referee rounds — the site definition alone was wrong three times (per-subtree budget; a
non-unique "sum over budget" predicate; a DCA that need not contain the range). It is the
sharpest-edged text in the spec; any future edit there should be re-refereed.

### 4.5 Ordering

Every state-word primitive carries a named ordering constant (DEC-045/069): claim `fetch_or`
acquire, release `fetch_and` release, count edges release (pairing with the *next claimer's*
acquire, not any reader load), mark release, slot publish release-store, reader slot loads
acquire via the protected-link primitive, descriptor CAS release with `protect` on bucket loads,
refcount add relaxed (legal only under an existing guarantee) with release/acquire-at-zero on
the decrement side, pool push release / pop acquire (single-consumer by construction).

The reason for the ceremony: the claim→slot-read edge and the descriptor-publish edge fail in a
way **the entire test matrix is structurally blind to** — x86's `lock` RMWs are full barriers,
so no QEMU config can expose a missing acquire, and ARMv8 TSan cannot either, because every
access involved is a well-formed atomic and TSan does not model the absence of an ordering.
Named constants plus review are the only detectors, which is why they are named rather than left
to convention.

### 4.6 Progress — what is claimed and what is deliberately not

The write path claims **no rung of the non-blocking hierarchy** (DEC-057). A claim bit is a
try-lock held across commit; a conflicting writer aborts rather than waits; there is no helping.
Strict lock-freedom is *refuted*, not unproven — suspend any claim-holder and every overlapping
writer blocks forever — and three successive attempts to claim lock-freedom during design each
turned out false. What is claimed instead: **no writer ever waits on another writer** (conflicts
cost a bounded abort-and-retry); **deadlock-freedom** from the total acquisition order via the
maximum-holder lemma (the holder of the lexicographically maximum *writer-held* site never fails
against a claim any writer holds — with the transient-phantom and terminal-mask cases carefully
carved out as bounded interference, not counterexamples); and **bounded hold and interference in
the deployed execution model** (non-preemptible, non-faulting writers clear every bit in bounded
cycles; residual timing-aligned livelock is excluded probabilistically by randomized backoff).
Individual starvation is possible and unbounded, accepted with backoff as the sole mitigation
(DEC-097) — every bound-adding mechanism catalogued lands on either a queue of writers waiting
on writers or the global lock the design exists to remove.

Acquisition order is (conceptual-tree level, ascending VA), top-down, where level is derived
from the slot span — the only measure invariant under cluster growth; step-counting from either
root shifts by one at a growth and lets two writers disagree about the order of the same two
nodes (DEC-037/053). The order carries two properties: the dying mark stays the one irreversible
step (a reclaimer that marked a child then failed on the parent bit has poisoned the range
forever), and the deadlock-freedom argument itself. The design's waits are exactly two —
teardown's grace-period barrier and the record-pool replenish — both on grace machinery over
bounded reader sections, neither a writer waiting on a writer.

---

## 5. Readers

A descent runs inside one `ReadGuard` — one section per descent, never per level. Every slot
link load goes through the tree's protected-link-load primitive: `kernel::rcu::protectWord`
supplies the named acquire and the debug section assert (the typed `protect` cannot type a
tagged codec word), the codec decodes, `SafePtr` wraps — this composite *is* this consumer's
`RcuPtr`, and the framework deliberately does not grow a general one (RCU-DEC-044, DEC-073).

A lookup returns a **counted reference** plus a **validity token**, and conflating their
lifetimes is a memory-safety defect (DEC-015/051/070). The counted reference keeps the record
alive after the guard closes — which exists because *a reader must close its section before
blocking*: EBR stalls reclamation domain-wide while any section is open, so a guard held across
a pager IPC would halt every CPU's reclamation indefinitely. The token — the VA plus the decoded
`(Mapping identity, absolute range)` — guarantees nothing by itself: the caller re-opens a
section, re-descends, and compares. It carries the *decoded* answer because slot words are
level-relative (the same 64 bits mean different ranges at different levels, so raw-word
comparison can compare equal across a halving), and it carries **no node pointer** because the
node can be reclaimed and its slab slot recycled during the pager round-trip — revalidation
through a stored pointer could compare equal against an unrelated live object.

Honestly stated boundary: **revalidation narrows the race; only an install-side interlock closes
it** (DEC-070). Writers never wait on readers, so a shrink or `mprotect` split can commit
between a successful compare and the PTE install made on its strength — W^X defeated after
`mprotect` returned. The VMM must re-check under a lock shared with its PTE-removal path
(Linux's speculative-fault discipline, where the seqcount recheck runs under the page-table
lock the zap path takes). That interlock is VMM machinery and is exported as a constraint.

### 5.1 The descent cache

Per-CPU, direct-mapped, provisionally 4 entries indexed by VA bits; each entry
`{AS generation, VA range, PinnedNode}` (DEC-079). For a cached node, neither mechanism
suffices alone and together they are exactly sufficient (DEC-016): the counted reference
prevents the memory being freed and recycled into an unrelated node (ABA); the dying mark tells
a hit the node was detached. The mark — not the reference — is what makes a cached node's
*children* safe to read, which is why every unlink path marks, teardown included.

Install is VA-range-deferred with a **per-entry** candidate register: a miss records a candidate
range, a second miss inside it installs during that descent's section — so nothing but an
address ever crosses a section boundary, and the pathological alternating pattern (which would
make the cache strictly worse than no cache) cannot defeat installation. A `Detached` or
generation-mismatched hit releases the pin, empties the entry, and counts as a miss. Teardown
does no cache work: a dead address space's entries are bounded residue
(`processorCount() × entries` nodes, ≈10–20 KiB on target hardware — DEC-096), evicted by
ordinary replacement and made harmless by the never-recycled 64-bit generation, because forcing
eviction would take exactly the cross-CPU IPI the project bars from correctness duty.

The core/policy seam (DEC-020) puts the whole concurrency protocol inside a core tree — one per
cluster, pure prefix indexing over an aligned span — with the cluster table, placement,
`Mapping` records and cache on the policy side. The cache is the seam's one genuine leak, closed
by an opaque API: `pin` (inside the observing section), `resumeDescent(pin, key) → result |
Detached`, `release` — the policy layer never dereferences the handle (DEC-078). The core also
exports **chunked ordered enumeration** — decoded `(value, absolute range)` pairs, ascending VA,
a bounded number of slots per read section resumed by VA cursor — the primitive span operations
iterate with (DEC-083).

---

## 6. Lifetime and reclamation

### 6.1 Two authorities, composed

Reclamation is **refcounted on top of RCU, not performed by it** (DEC-035): a counted-reference
mechanism owns each node and `Mapping` and calls `destroy` at zero; the RCU deleter at
grace-period end *releases* the structural reference rather than freeing. This composes only
because **every release is deferred** — the natural implementation (release children during the
commit walk, which already visits every node) is a use-after-free with no grace period at all.

The deleter/destructor split is load-bearing, not cosmetic: a node's **deleter** (grace-period
end) releases the `Mapping` references its leaf slots hold; its **destructor** (refcount zero,
possibly much later, under a foreign CPU's cache pin) releases *nothing* — otherwise a
cache-pinned retired node would pin every `Mapping` it named, transitively VMObjects and frames,
behind a bound stated in nodes. Child-node slots release nothing from either (each child has its
own retire); nothing recurses. The clean formulation: a subtree detach is equivalent to clearing
every slot and unlinking every node, executed as one parent-slot store — every displaced value
then follows the rules an individual clear already has.

### 6.2 `Mapping` counting

One structural reference per leaf slot that names the record, maintained **per naming-slot
transition, not per operation** (DEC-048): +1 in the commit phase before each slot's publish,
−1 deferred when a slot stops naming it. The per-transition form is the point — an enumeration
of operation events was tried and is not exhaustive (initial placement into *k* slots is +k; a
middle punch is +(k−1) where k is the survivor-leaf count, ~45 in the wide-spine shape; an edge
subdivision nets zero; an in-place shrink moves nothing). The record is constructed at count
zero; the first publish takes it to one.

**Every structural +1 is a commit-phase step** (DEC-067), which is what makes aborts count-clean:
an abandoned attempt crossed no boundary, took no increments, and has nothing to undo — where
the natural reading (take the reference while building the child during acquisition) leaks a
pinned record, VMObject and frames on every abort, invisibly. The one exception is cluster
growth, whose publish is a losable CAS: the grower increments before it and a loser reverts
synchronously — safe there and only there because the reference was never published and the
descriptor's own reference holds the count above zero.

### 6.3 Deferred releases: the `DeferredRelease` pool

A deferred `Mapping` release needs a retire subject, and the record cannot be its own —
`Mapping` is N:1, so two CPUs clearing two slots naming one record would enqueue one intrusive
linkage twice: a lost or doubled release, deterministic in a single-threaded three-slot
`munmap`. So each deferred release rides its own small record, `{Mapping*, delta, homePool*,
RetireHead}`, drawn during acquisition from a **per-CPU pool of recycling records** — never
allocated on the path (DEC-068). The binding reason: allocating there would make `munmap` — the
operation that *relieves* memory pressure — the one that fails under it.

The records **recycle and are never freed**: a record's deleter releases and pushes the record
back to its home pool, so steady-state churn allocates nothing. The per-CPU population is fixed
at address-space creation at the per-operation ceiling — an **edge-sum** bound (≈230 fully
grown; the topmost node's valence plus two partially-covered edge nodes per level at valence−1
each), which is a different governing quantity from the claim-site bound and `static_assert`ed
from the geometry. Cost: ≈14.4 KiB realised per CPU per address space; whether eager worst-case
sizing is right, or a smaller reserve behind the fallback suffices, is the measurement-gated
ITEM-084.

A short pool means the owner's own retirees are in flight (only the owner draws). The remedy is
deliberately sequenced: **abandon the attempt first** (a grace wait inside the attempt's own
section deadlocks on itself), then pump `tryAdvance` and drain between attempts, then call
**`barrier`** — not `synchronize`, which promises only that a grace period elapsed and does not
seal the caller's open bag; `barrier` seals, rotates and drives the owner's retirees home, after
which the retried draw succeeds deterministically. The blocking primitives carry the strict
no-`#PF` mask, which is fine because record-drawing operations are displacement operations in
syscall context; fault-path work draws no records.

Freshness discipline (the DEC-047 stale-TLB bug class): `onPreTouch` covers only the
`RetireHead`, so every access a deleter makes beyond it — node slots, the `Mapping` count word,
the record's own fields — goes through `SafePtr`/`ensureTLBEntryFresh`, as does each record's
first draw by its owning CPU. The pool *heads* are exempt for a structural reason: they live in
the control block's pinned storage (§7.1), whose mapping never changes.

### 6.4 Reclamation progress

There is no reclamation daemon; progress is pulled. Three pump sites (DEC-060/071): **operation
exit** (a mutation that retired anything pumps after closing its section, so the just-filled bag
becomes drainable by any CPU rather than sitting open on a CPU that may never mutate again),
**the fault path** (runs on every CPU that touches the tree), and **teardown**
(`drainAllQuiescent`). Every pump is bounded by `drainBatchBound` (provisionally 64) because an
unbounded drain inherits arbitrarily many foreign deleters on a path a `#PF` handler sits on —
and for that product-of-constants argument to hold, one constraint is exported: **VMObject
teardown must itself be deferred or incremental**, or one large `munmap` turns some later CPU's
page fault into an unbounded frame-free run with the batch bound bounding the wrong quantity
(deleter count, not deleter cost).

---

## 7. The address-space lifecycle

### 7.1 The control block

Every per-address-space object anchors in one control block: root-page pointer, RCU domain
handle, per-CPU `DeferredRelease` pool array and current-cluster cells, the `dying` flag, and a
monotonically-assigned, never-recycled 64-bit generation (DEC-082). It lives in **pinned
`reservePerDomainStaticBuffer` storage beside the RCU domain block** — deliberately not
vmsmalloc memory, because its generation, flag and pool heads sit on other CPUs' hot paths, and
a vmsmalloc block would put a TLB-freshness call on every descent-cache hit. Pinned storage's
never-changing mapping removes the obligation outright.

**Creation is an explicit sequence with a reverse-order unwind** (DEC-101): control-block
reservation (its own brief acquisition of the exported `DomainManagementLockGuard`; zeroed;
null → `ENOMEM`) → `Domain::init` (which takes the same lock *internally* — two independent
acquisitions, never one spanning hold, which would self-deadlock) → root page → per-CPU record
pools (individual `tryMake`s) → release-store publish of the block, with threads created only
after the publish so thread creation's happens-before edge orders every other CPU's first sight.
Any null unwinds in reverse **through `deinit()`** — skipping that step leaks the pinned domain
reservation permanently and silently breaks vmsmalloc's high-water-mark accounting.

### 7.2 Teardown

The full sequence: *dying flag → thread destruction → `synchronize` → walk → `drainAllQuiescent`
→ free the root page and record pools → `deinit` → return the control block to its freelist.*

The barrier makes the walk genuinely uncontended (DEC-065): every attempt checks the flag at
attempt start inside its section, so `synchronize` returning means every attempt that could have
missed the flag has exited and every later one abandons. This is load-bearing against one
specific hole: a merely-tolerated in-flight attempt could grow a cluster through the bucket
CAS — the one publish site with no claim interlock — behind a bucket the walk already read,
leaving a live unmarked node behind teardown: a leak plus a descent-cache use-after-free.

**The walk is a sequence of ordinary detachment units storing empty** (DEC-100): per cluster,
deepest first, claim → mark → **unlink (store zero into the unit root's parent slot)** → retire,
one read section per unit, releasing the parent-slot bit at unit end. The unlink-before-retire is
rcu.md's own writer obligation, and it is what makes the multi-section walk sound: between units
the walk holds only a VA cursor, and a re-descent crosses only live links because everything
retired is already unreachable. Two fatals forced this shape: the earlier walk retired nodes
still linked (a retire's own synchronous drains could destroy children live parents still
pointed at), and it double-released every cluster root — the root's single structural reference
is descriptor-held, and both the root's own deleter and the descriptor's deleter released it.
Now **the walk never retires a cluster root**: each cluster's final unit claims and marks the
empty root, clears the bucket word, and retires the *descriptor*, whose deleter is the root's
only releaser.

Teardown claims and marks like every other path — no exemptions — because an exemption is how
the mark-site assert gets deleted from the paths where it is load-bearing; the uncontended
walk's `fetch_or` prior *proves* quiescence (`prior & (valenceMask | MARK)` nonzero is a
protocol violation, debug-asserted). The whole sequence is gated on ITEM-050's open half: a
pager-blocked thread has no cancellation path yet, so a hostile pager can hold thread
destruction — and everything after it — open indefinitely. Residue after teardown is bounded,
not zero, and the bound is the answer (DEC-096), because forcing invalidation needs an IPI the
project bars from correctness.

---

## 8. Placement policy

**Free-range selection is random probe and verify** (DEC-007): K random probes (provisionally
8), then one bounded free-run scan, then a distinct `kNoSpaceInCluster` result (DEC-095). No gap
augmentation — a largest-free-gap per subtree (Linux's `rb_subtree_gap`) would force every
operation to propagate to the root, reintroducing exactly the contention this design removes.
The scan is **chunked** (bounded slots per read section, VA-cursor resume — an unchunked
whole-cluster descent is ~10³ nodes of EBR-stalling non-preemptible work) and its result is
defined as "no free run *observed by this scan*": a run freed behind the monotonic cursor is an
accepted false negative costing an unnecessary growth or extra cluster, never a wrong mutation.
Probe occupancy is per-cluster, not per-address-space — a process with a few hundred MiB in a
1 GiB cluster probes at tens of percent, so the scan is a priced slow path, not a rare fallback.

**Cluster assignment defaults to per-CPU** (DEC-091, settled by user sign-off): each CPU carries
a current-cluster cell per address space — holding the **bucket index**, never a descriptor
pointer, which growth retires — creating its cluster on first use and re-pointing on
out-of-space. Per-CPU wins on concurrency (disjoint clusters per allocating CPU, the vmsmalloc
arena shape), NUMA homing (the faulting CPU is the allocating CPU), and boundedness; its known
weakness is single-threaded ASLR (one cluster → a leak costs 14 bits of conditional entropy).
**The binding constraint from sign-off: assignment stays behind a swappable policy seam** above
the explicit-cluster mechanism — per-CPU, random-among-existing, or a hybrid must each be a
policy-object swap touching no mechanism code.

Sizing: fixed 1 GiB initial root; a request larger than the C0 span roots its cluster at the
smallest covering level — necessity, not adaptivity (DEC-090). A fixed-address or oversized
request crossing a bucket boundary is **decomposed by the VMM into one mapping per bucket**
(same VMObject, successive offsets) before the tree is involved (DEC-080) — the alternatives
were letting mappings span clusters (dismantling the one-cluster-per-bucket invariant and the
site bound in one stroke) or an `ENOMEM` on an invisible 256 GiB internal boundary. This also
gives >256 GiB mappings their representation. Genuine exhaustion — every bucket's cluster full,
growth can't help — surfaces as `ENOMEM`: real VA exhaustion, not a policy gap.

Placement granularity (64 KiB, policy) and the resolution floor (4 KiB, capability) are separate
numbers — Windows has shipped exactly this split since NT — because POSIX page-granular
`mprotect` must be representable while normal mappings get the full memory win.

---

## 9. Cross-spec contracts

This design forced amendments to its neighbors, all applied and adversarially reviewed:

**`kernel::rcu`** — per-address-space **domain lifecycle** (RCU-DEC-043): rare, lock-serialized
`init`/`deinit`; `init` zeroes recycled blocks (zero-fill is per-*reservation*, and a recycled
block otherwise carries the prior tenant's `teardownActive`/`inDrain` state — silent
no-reclamation); `deinit` debug-poisons *then* clears `initialized` (that order — the poison
would otherwise leave the nonzero-checked bool reading true); install-before-publish makes the
pinned-storage fault-free premise survive runtime creation; the **`DomainManagementLockGuard`**
is exported for consumer-side reservations. **`protectWord`** (RCU-DEC-044) is the raw-word
sibling of `protect` for encoded links. The deleter contract (RCU-DEC-045) gains two exceptions —
releasing counted references the retired object holds, and returning the object to an
allocator-shaped free store — plus a pinned-storage exemption from the freshness discipline.

**`vmsmalloc`** — failable allocation (`vmsmallocTry`/`tryMake`, DEC-048: both panic sites
patched, so untrusted userspace creating live sparse mappings can no longer panic the kernel and
`mmap`/`fork` surface `ENOMEM`); the {192, 320} classes with contractual 64 B alignment
(DEC-049); runtime `reservePerDomainStaticBuffer` under the domain-management lock (DEC-050)
with the hardening DEC-051 added when review found the relaxation had silently invalidated three
init-time premises: a failable try-variant (fork cannot reach the panic), the write-once
not-present→present argument for why runtime installs need no shootdown, and the lock-scope
statement for the same-entry install race.

**Exported constraints on future specs** (the VMM/VMObject specs inherit these): VMObject
teardown deferred or incremental; the install-side interlock for fault revalidation; rmap lives
in the VMObject with bounded deregistration and lock-plus-counted-reference traversal (a bare
cross-address-space dereference races a dying space's count-zero destroy); shootdown tracking
lives in the `Mapping`, maintained at fault-commit, encoding open; a `Mapping`'s destruction
touches no per-address-space storage (which is what makes a pager-pinned straggler destroy safe
after teardown); bucket-straddle decomposition happens above the tree.

---

## 10. What is deliberately not promised

- **Whole-region atomicity.** A reader may observe a mix of old and new across a multi-slot
  operation; range operations decompose per cluster (DEC-058) and per detachment unit (DEC-077).
  Per-address hole-freedom *is* promised: an address mapped before and after is never observed
  unmapped during.
- **Any rung of the non-blocking hierarchy** (§4.6 above).
- **Unconditional disjoint-operation non-conflict** — conditional on prior expansion, with a
  counter watching it.
- **Zero teardown residue** — bounded by remote cache turnover instead.
- **A starvation bound beyond randomized backoff.**
- **Scan exhaustiveness** — `kNoSpaceInCluster` is a property of one scan, not the cluster.
- Value-based mapping merging, VMObject internals, hardware page tables, shootdown policy, and
  swap are other specs' problems; the tree's business ends at naming the mapping.

## 11. Open items (all gated, none blocking implementation start)

| Item | Question | Gate |
|---|---|---|
| ITEM-084 | Pool sizing: eager ≈230/CPU/AS vs smaller reserve + `barrier` fallback | Phase 3/5 draw-count histogram |
| ITEM-055 | State-word placement vs false sharing (288→320 B is free) | Phase 2 `-icount` measurement |
| ITEM-047 | `fork`'s point-in-time view. Leading shape after the rwlock's rejection (shared-line contention — the paper's own bottleneck): a reversible teardown-style barrier — `forkBarrier` bit beside `dying` (zero added mutation-path work), `synchronize`, enumerate-copy, writers park in the scheduler; faults run concurrently with the copy | VMM spec (owns CoW + enumeration choreography) |
| ITEM-050 | Pager cancellation — an unresponsive pager pins references and holds teardown open | External (pager design) |
| ITEM-002 | Bit assignment below the root (5/5/4/4 is one arrangement) | Calibration via the geometry parameter |
| ITEM-004 | Worst realistic `Mapping`/VMObject fan-out — does a refcache-shaped subsystem exist | Measurement; do not close by assumption |
| ITEM-031 | Compress leaf slots only, leaving child pointers plain | `-icount` measurement |
| ITEM-024 | Confirm the fused probe-verify-install is actually one traversal | Measurement |

## 12. Verification strategy, briefly

A userspace harness (mock `VMSubstrate`, ASan + ARMv8 TSan runners) against a serialized
reference implementation — a shadow interval map that must model the codec, since sub-range
cover is unanswerable from tree shape alone. The geometry parameter makes the concurrency model
check *exhaustive* on a tiny geometry rather than statistical on the real one. The lifetime
layer is untestable without the poisoned-arena UAGP oracle (attached to refcount-zero `destroy`,
not the deleter), so that instrument is Phase 0, before any feature. Three structural blind
spots are named rather than papered over: missing acquires (x86 full-barrier RMWs + TSan's
atomics blindness — carried by named constants and review), the compressed codec's arithmetic
(the harness runs the uncompressed codec by necessity; the worst case, offset-zero, is now
structurally impossible via the guard bit), and intra-arena leaks (LSan sees one live mmap —
explicit node accounting instead). Five implementation phases: oracle → single-threaded core →
concurrent core → placement + lifecycle → descent cache → in-kernel stress, CI-gated through
`kernel::exitToHost`.

---

*Review pointers: the sharpest text is §6.5 (decomposition — seven rewrites), §7.4 (teardown
units), §6.6/§6.7 (orderings and the progress taxonomy), and §7.1–7.2 (the deleter/destructor
split and per-transition counting). The two decisions with real recorded alternatives are
DEC-091 (assignment policy — settled, seam mandated) and DEC-093 (geometry — settled,
constexpr-only retuning mandated); ITEM-047 (fork) is the one open design conversation.*
