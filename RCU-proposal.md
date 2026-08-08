# RCU Framework Proposal for CroCOS

> **Status:** PROPOSAL — not yet a spec. Decision-shaped items are marked
> PROPOSED (recommendation made, open to challenge) or OPEN (genuinely
> unresolved; needs a decision before spec drafting). The intended consumer is
> a RadixVM-style radix tree for the virtual memory manager.

## 1. Why RCU, and why now

The next major memory-management milestone is RadixVM: a radix tree mapping
virtual addresses to mapping metadata, with lock-free lookups on the page-fault
path and concurrent mutation by other cores. The hard problem in any such
structure is not the lookup or the insert — it is *safe memory reclamation*:
when a writer unlinks a node (mapping removal, subtree collapse), some reader
on another CPU may still be traversing it. Something must guarantee the node's
memory is not recycled until all such readers are done.

`Core::TreiberStack` explicitly declares reclamation out of scope ("node
lifetime is the consumer's responsibility"); vmsmalloc sidesteps it with
immortal slabs (DEC-002). RadixVM cannot sidestep it: nodes must be freed and
their slots reused, and readers must never observe a freed node. This proposal
defines the reclamation framework — an RCU-shaped grace-period mechanism —
as its own subsystem, designed and tested independently of the tree.

### Hard constraints (from the project, not negotiable in this design)

- **C1 — Tickless kernel.** There is no periodic timer interrupt. Grace-period
  detection cannot be driven by tick-time sampling of per-CPU state (the
  classic Linux RCU model). Idle CPUs may sit in `hlt` indefinitely and must
  not stall grace periods.
- **C2 — No scheduler dependency for correctness.** There is no scheduler yet,
  and when one exists the framework may consume *hints* from it (reclamation
  latency, placement) but must remain *correct* without it. Context switches
  cannot be the quiescent-state signal.
- **C3 — Testable in isolation on the dev machine.** The framework must build
  and run in the userspace test harness (ARMv8 M1, TSan as the primary release
  gate, ASan runner alongside), following the vmsmalloc Phase 8 pattern:
  real kernel headers under `CROCOS_TESTING`, `std::thread`-per-CPU with
  `bindThreadToCpu`, mock CpuLocal and mock substrate.
- **C4 — Freestanding kernel C++26.** No std library; `Core::Atomic` plumbing,
  no exceptions/RTTI, camelCase naming, debug-only runtime checks per the
  kernel safety stance.
- **C5 — IPIs as garnish, never load-bearing.** Limited, exceptional IPI use
  is acceptable, but only asynchronously — fire-and-forget nudges with no
  waiting for delivery or response — and never as a mechanism the framework
  leans on heavily. Concretely: correctness and safety must hold unchanged if
  any IPI is dropped, delayed indefinitely, or never sent; IPIs may only
  shorten *reclamation latency*. (The IPI subsystem is minimal today only for
  lack of a consumer; this framework would be its first real one.)

## 2. Review — what the existing MM architecture gives us

A survey of the infrastructure the framework can build on, and the
CroCOS-specific properties that shape the design.

**Physical / virtual substrate.** `PageAllocator` (per-NUMA-domain, per-CPU
pools) → `VMSubstrate` (a dedicated 512 GiB VA window at root[510], per-CPU
arenas, `allocPage`/`freePage`/`reclaimSlabPage`) → `vmsmalloc`
(magazine-and-depot slab allocator, per-CPU magazines in `CpuLocal`,
per-domain `ChainedTreiberStack` depots) → the public `VMSubstrate::make<T>` /
`destroy<T>` returning `SafePtr<T>`. Radix tree nodes will be `make<T>`
allocations; RCU's deferred destruction will ultimately funnel into
`destroy<T>`.

**The no-IPI TLB model is load-bearing here.** VMSubstrate propagates PTE
changes without shootdowns: a CPU that changes a mapping sets per-page dirty
bits for all other CPUs; `SafePtr<T>` dereferences call `ensureTLBEntryFresh`,
which checks the bit and `invlpg`s locally. Two consequences for RCU:

1. *RCU does not replace TLB freshness.* A reader that acquires an
   RCU-protected pointer still needs the `SafePtr` discipline to see current
   page *contents* on this CPU. The radix tree must store child links in
   `SafePtr`-shaped form. These are orthogonal protections: RCU guarantees the
   object is not recycled; `ensureTLBEntryFresh` guarantees this CPU's view of
   its bytes is current.
2. *Failure containment is unusually good.* `vmsfree` never unmaps (slabs are
   immortal; `reclaimSlabPage` remaps onto a read-only sentinel). A
   use-after-grace-period bug therefore reads poisoned-or-recycled bytes
   rather than faulting unpredictably — and the debug-build `0xCC` poison
   (DEC-024) plus the torture suite's own poison make such bugs loud in
   testing.

**Per-CPU execution context.** `kernel::cpuLocal()` (GSBase → `CpuLocal`
struct) is valid from very early boot on every CPU, with
`getLogicalProcessorID()` as the portable CPU-identity accessor. The userspace
harness already models this with a thread_local base pointer. This is exactly
the "reader slot" identity RCU needs, with a tested mock.

**Interrupt-context tracking.** `InterruptContextDepths` +
`InterruptContextGuard` give per-CPU nesting counters per interrupt kind, and
vmsmalloc's DEC-014 already defines a forbidden-context discipline
(IRQ/NMI/#GP/#MC/#UD/#DF forbidden; #PF conditionally legal). RCU's context
rules should *align with* vmsmalloc's, since deferred deleters call `vmsfree`.

**NUMA-aware static storage.** `reservePerDomainStaticBuffer(byteSize, d)`
provides init-time, NUMA-pinned, kernel-lifetime buffers — the right home for
per-CPU RCU slot state (each CPU's slot in its local domain's memory), exactly
as Phase 3 did for the vmsmalloc depots.

**Concurrency precedents to follow.** DEC-042's named-memory-ordering-constant
policy (every ordering pinned at a named constant; downgrades are one-point
edits and audit triggers); the Phase-10 single-runner-via-try-lock pattern;
DEC-036's "forward progress driven from the hot path's slow path, not from a
daemon" pattern — directly reusable as RCU's pull-based grace-period
advancement (C1/C2 forbid a reclamation daemon anyway, for now).

**Timing.** `timing::monoTimens()` exists and is scheduler-free — usable for
debug-build grace-period-stall and overlong-critical-section diagnostics, but
not required for correctness.

## 3. Requirements on the framework

Derived from the RadixVM consumer and constraints C1–C4:

- **R1 — Read side is cheap and wait-free.** A page-fault-path lookup is one
  read-side critical section wrapping a 3–4 level pointer chase. Per-section
  overhead must be O(1) CPU-local work (target: a couple of CPU-local atomic
  ops plus at most one full fence), with *no* stores to shared cache lines and
  no CAS loops.
- **R2 — Read side is legal in any context.** Page faults, IRQ handlers, NMI
  — entering/exiting a read-side critical section must be safe anywhere,
  including nested within itself (interrupt arriving mid-section on the same
  CPU).
- **R3 — Grace periods complete without universal participation.** A CPU that
  is idle, halted, or simply never touches the subsystem must not delay grace
  periods (C1, C2). Only CPUs *inside* read-side critical sections may block a
  grace period, and only until they exit.
- **R4 — Deferred reclamation with bounded writer cost.** Writers retire
  objects (O(1), CPU-local enqueue) and the framework invokes a deleter after
  a grace period. Blocking `synchronize()` exists for slow paths but the
  workhorse is `retire`.
- **R5 — No-ABA-within-a-section guarantee.** A pointer loaded inside a
  read-side critical section refers to an object whose memory is not recycled
  until after the section ends. This is what makes the tree's lock-free read
  algorithm simple (no per-node revalidation).
- **R6 — Multiple independent domains.** The radix tree gets its own RCU
  domain so its grace periods don't couple to future consumers', and so a
  domain can be constructed/torn down in a unit test.
- **R7 — Engine testable as a pure algorithm.** The grace-period engine must
  be expressible over `Core::Atomic` and an abstract slot array, with zero
  dependencies on kernel headers — unit-testable and torture-testable in
  userspace, TSan-clean on ARMv8 (C3, C4).
- **R8 — Scheduler/timer integration is additive.** Future hooks (idle entry,
  context switch, timer wheel) may *accelerate reclamation* but must never be
  required for safety, and their absence must only cost memory latency, never
  correctness (C2).

## 4. Design space

Per project convention, the full catalog before the recommendation.

### A1 — Tick-driven quiescent-state RCU (classic Linux non-preempt RCU)

Readers cost nothing; quiescent states (context switch, idle loop, user-mode
return) are observed by the tick and aggregated by softirq machinery.
**Verdict: structurally unavailable.** Requires both the tick (violates C1)
and the scheduler's quiescent points (violates C2). Cataloged because it
explains *why* general-purpose RCU usually looks scheduler-shaped, and why
ours cannot.

### A2 — Explicit QSBR (liburcu-qsbr model)

Readers are free; every participating CPU must periodically call
`quiescentState()`, and CPUs going idle must declare themselves offline.
Strongest possible read side (zero overhead). **Costs:** correctness becomes a
*whole-kernel discipline* — every long-running kernel path must remember to
announce quiescence, and idle/halt paths need dyntick-style offline
bookkeeping. A single forgotten loop stalls every domain. This is the
anti-modular failure mode: correctness of the radix tree would hinge on code
nowhere near the radix tree. Note that C5-style async nudge IPIs do *not*
rescue QSBR: with unmarked readers, an IPI handler interrupting arbitrary code
cannot tell whether the interrupted context is mid-read, so it can never
safely announce quiescence on the target's behalf — quiescence still requires
the target's own code to reach an announcement point, which is exactly the
whole-kernel discipline being objected to. **Verdict: rejected as the
foundation** — but
note that because the consumer-facing API proposed below is engine-agnostic,
a QSBR-style fast-path engine could later back specific domains once a
scheduler provides natural quiescent points (R8), without consumer changes.

### A3 — Two-phase reader counters (SRCU-shaped)

Per-CPU `lockCount[2]`/`unlockCount[2]` arrays; readers increment the current
phase's lock count, remember the phase, increment its unlock count on exit.
Grace period: flip the phase, wait for the old phase's sums to balance, twice.
Scheduler-free, tickless-safe, satisfies R1–R7. Its one structural advantage
over A4: *migration tolerance* — a reader may lock on one CPU and unlock on
another, which is what makes Linux SRCU sections sleepable/preemptible.
**Costs:** the grace-period algorithm is subtler (double flip, sum-balance
argument across split counters), and the migration tolerance buys nothing
under our reader contract (sections are non-blocking and CPU-pinned — see
§5.4). **Verdict: viable runner-up.** Rejected only because A4's invariant is
materially simpler to reason about, model-check, and torture-test — and
simplicity of the safety argument is worth more than an unused degree of
freedom. Revisit if preemptible read-side sections are ever wanted (§8).

### A4 — Epoch-based reclamation (EBR; Fraser, crossbeam-epoch lineage)

A monotonically increasing 64-bit global epoch per domain. Readers, on
entering an outermost section, snapshot the global epoch into their CPU-local
slot and mark it active (one store + one full fence); on exit, mark inactive.
The epoch may advance only when every *active* slot's snapshot equals the
current epoch. Objects retired at epoch `e` are reclaimed once the global
epoch reaches `e + 2`. Satisfies R1 (one fence, CPU-local stores only), R2
(slot state nests trivially on one CPU), R3 (inactive slots are invisible to
the scan — idle CPUs cost nothing, no dyntick bookkeeping), R4 (per-slot
epoch-tagged limbo bags), R5 (the e+2 rule), R6/R7 (the engine is ~200 lines
over atomics and a slot array). The safety argument is short and standard
(§5.6). **Verdict: recommended.**

### A5 — Hazard pointers

Readers publish each pointer they are about to dereference; reclaimers scan
hazards. Bounded unreclaimed memory and per-object (not per-epoch) blocking —
a stalled reader blocks only the objects it actually holds. **Costs:** a
store + fence *per node visited* plus a validation re-load — hand-over-hand
protocol through every radix level, easily 4× the synchronization cost per
lookup and a far more invasive tree algorithm. Wrong trade for a read-mostly
deep-traversal structure. **Verdict: rejected for this consumer**; remains the
right tool if some future consumer needs unbounded-duration references.

### A6 — Per-node reference counts

Atomic refcount per node, incremented along the traversal path. Shared-cache-
line contention on the root and upper levels — precisely the scalability
failure RadixVM exists to avoid. **Verdict: rejected.**

### A7 — Type-stable nodes + version validation (no reclamation protocol)

Exploit vmsmalloc's immortal slabs: never free node *memory*, recycle nodes
through a free list, and have readers revalidate a per-node version/generation
counter at each step (the `SLAB_TYPESAFE_BY_RCU` pattern). **Costs:** ABA
moves from "impossible within a section" to "the common case the tree must
handle" — every traversal step needs version re-checks and every structural
operation needs a proof about concurrent re-validation; per-type memory
high-water-mark never shrinks. This trades a one-time framework for permanent
complexity in every consumer's read algorithm. **Verdict: rejected** — though
it is the natural *fallback semantics if a bug escapes*: thanks to immortal
slabs, even a UAGP bug reads recycled-typed memory, not unmapped memory.

### A8 — Seqlock / retry-based readers

Writers bump a sequence; readers retry on conflict. Retry storms under write
bursts, writers starve readers of forward progress on the fault path, and
deep multi-node traversals need snapshot-consistency machinery. **Verdict:
rejected** (though seqlocks may complement RCU for small leaf *payloads*
later — a per-mapping-entry seqlock under an RCU-protected node is a
reasonable future pattern).

### Recommendation

**A4 (EBR) as the engine, behind an engine-agnostic consumer API.** It is the
only candidate that simultaneously: needs nothing from scheduler or tick (C1,
C2), leaves uninvolved CPUs completely out of the protocol (R3), keeps the
reader contract trivial for the tree (R5), and reduces to a small,
slot-array-plus-atomics algorithm that runs under TSan on the M1 unmodified
(R7). Its known weakness — a stalled reader stalls the domain's reclamation
(not its correctness) — is shared by every grace-period scheme including
Linux RCU, and is bounded here by the non-blocking reader contract plus
debug-build stall detection.

## 5. Proposed design

### 5.1 Layering

```
libraries/Core/include/core/rcu/EpochDomain.h     ← the engine (R7)
    Core::rcu::EpochDomain     — epoch counter + slot scan + advance logic
    Core::rcu::ReaderSlot      — one CPU's state (cache-line aligned)
    Core::rcu::RetireHead      — intrusive deferred-free node (rcu_head analog)
    Core::rcu::LimboBag        — per-slot, epoch-tagged retire list

kernel/include/rcu/RCU.h                          ← the kernel veneer
    kernel::rcu::Domain        — EpochDomain + NUMA-placed slot storage
    kernel::rcu::ReadGuard     — RAII section bound to cpuLocal() identity
    kernel::rcu::retire(...)   — enqueue to this CPU's bag; opportunistic drain
    kernel::rcu::synchronize() — blocking grace period (slow paths only)
```

The Core engine knows nothing about CPUs, interrupts, or vmsmalloc: it is
parameterized by a slot count and a slot-array pointer supplied by its owner.
The kernel veneer supplies storage via `reservePerDomainStaticBuffer` (each
CPU's slot resident in its local NUMA domain), binds slot identity to
`getLogicalProcessorID()`, and enforces context rules with debug asserts
against `InterruptContextDepths`. The userspace harness constructs the same
veneer over mock CpuLocal and heap storage — the vmsmalloc Phase 8 pattern,
unchanged (C3).

This mirrors the proven TreiberStack split: algorithm in Core, kernel-specific
binding in the kernel, one consumer-shaped API.

### 5.2 Per-slot state (one reader slot per possible CPU)

```cpp
struct alignas(64) ReaderSlot {
    // Read by remote grace-period drivers; written only by the owning CPU.
    Atomic<uint64_t> state;        // bit 0: active; bits 63..1: epoch snapshot
    uint64_t nesting;              // owning-CPU-only; plain (non-atomic)
    LimboBag bags[kBagCount];      // epoch-tagged retire lists, owning-CPU-only
    // counts / thresholds / stall-debug timestamps...
};
```

`nesting` and the bags are touched only by the owning CPU (sections and
retires are CPU-pinned, §5.4), so they need no atomics; interrupt-nested
sections on the same CPU are naturally serialized. Only `state` crosses CPUs.
Slot count is `arch::processorCount()` at domain init (≤
`arch::MAX_PROCESSOR_COUNT`); at one or two cache lines per slot a domain's
slot array is a few KiB.

### 5.3 The algorithms

**Read lock** (outermost; nested entries only bump `nesting`):

```
e = globalEpoch.load(kEpochLoadOnEnter)        // RELAXED
slot.state.store(makeActive(e), kStatePublish) // RELAXED store...
fullFence(kReaderActivationFence)              // ...then SEQ_CST fence
```

**Read unlock** (outermost): `slot.state.store(kInactive, kStateRetire)` —
RELEASE, ordering all section reads before the deactivation.

**tryAdvance** (any CPU, any time; concurrent callers harmless):

```
e = globalEpoch.load(ACQUIRE)
fullFence(kScanFence)                          // SEQ_CST, pairs with reader fence
for each slot s in [0, slotCount):
    st = s.state.load(kScanLoad)
    if (isActive(st) && epochOf(st) != e) return false
globalEpoch.compareExchangeStrong(e, e + 1, kEpochAdvance)  // failure is fine
drainOwnExpiredBags()
return true
```

Note the **exact-match** check: an active reader whose snapshot lags the
current epoch blocks advancement unconditionally. This is what makes arbitrary
delay between a reader's epoch load and its activation store safe (it can only
delay reclamation, never unblock it early). 64-bit epochs make wraparound a
non-issue (advances happen at grace-period rate).

**retire(obj, deleter)**: tag this CPU's current bag with the current epoch
(draining a stale-tagged bag first — its epoch is necessarily expired), push
the object's intrusive `RetireHead`, and if the bag count crosses a threshold,
call `tryAdvance`. This is the DEC-036 pattern: forward progress is driven
from the mutation path itself, no daemon, no tick (C1/C2). `kBagCount = 4`
(PROPOSED) gives slack between the epoch a bag was tagged at and the epochs
that may be current while it still holds entries.

**drain**: run deleters from own-slot bags whose tag `t` satisfies
`globalEpoch ≥ t + 2`. Deleters run on the retiring CPU — NUMA-friendly and
matches vmsmalloc's same-domain free preference.

**synchronize()**: spin on `tryAdvance` until the epoch has advanced twice
past its value at entry. Slow-path-only by contract (teardown, rare
structural ops); forbidden in interrupt context and inside a read-side
section (debug-asserted).

All orderings are named constants per DEC-042; the table above is the
*starting* assignment, to be pinned during spec drafting with litmus tests
(§7). The single full fence in read-lock is the entire hot-path cost (R1) —
one `dmb ish` per outermost section on ARMv8, amortized over a whole radix
walk.

### 5.4 The reader contract

A read-side critical section:

1. **does not block** — no `synchronize`, no waiting on other CPUs' work
   (standard RCU contract);
2. **is CPU-pinned** — begins and ends on the same CPU. Trivially true today
   (no scheduler); when preemption exists, sections disable preemption
   (DEC-030-style placeholder asserts now, real asserts later — the same
   forcing-function pattern vmsmalloc Phase 7 uses);
3. **is bounded** — debug builds stamp section entry with `monoTimens()` and
   warn/panic past a generous threshold, and the grace-period driver reports
   the slot ID of a stall (the rcutorture-stall-warning analog);
4. **may nest freely**, including from interrupt handlers on the same CPU
   (R2) — `readLock`/`readUnlock` are a handful of CPU-local instructions
   with no allocation and no loops, legal in any context up to and including
   NMI.

`retire`/`drain` follow vmsmalloc's context rules (DEC-014 as amended):
forbidden in IRQ/NMI/#GP/#MC/#UD/#DF context, conditionally legal in #PF —
this matters because the radix tree's #PF-path mutations will retire collapsed
nodes, and deleters bottom out in `vmsfree`. Aligning the two disciplines
means one rule for consumers, enforced by the same `InterruptContextDepths`
counters.

### 5.5 API sketch (consumer-shaped, engine-agnostic)

```cpp
namespace kernel::rcu {
    class Domain;                       // one per protected structure (R6)

    class ReadGuard {                   // RAII; non-copyable, non-movable
    public:
        explicit ReadGuard(Domain&) noexcept;
        ~ReadGuard() noexcept;
    };

    // Acquire-load of an RCU-published pointer. Debug-asserts that the
    // caller is inside a section on this domain. (TLB freshness is the
    // *caller's* job via SafePtr discipline — see §5.7.)
    template <typename T>
    T* protect(Domain&, const Atomic<T*>& src) noexcept;

    // Deferred destruction. T embeds a Core::rcu::RetireHead; the member-
    // pointer NTTP recovers T* from the head without RTTI or offsetof games.
    template <typename T, Core::rcu::RetireHead T::* Head>
    void retire(Domain&, T* obj, void (*deleter)(T*)) noexcept;

    void synchronize(Domain&) noexcept; // slow paths only
    bool tryAdvance(Domain&) noexcept;  // opportunistic; scheduler-hint entry
    size_t drainLocal(Domain&) noexcept;// run this CPU's expired deleters
}
```

The intrusive `RetireHead` (16 bytes: next + callback) is PROPOSED over
allocated tracking nodes: retiring must not allocate (it runs on free paths
and must not recurse into vmsmalloc's failure modes). Radix nodes carry the
head as a member — idle bytes during the node's live phase, repurposed at
retire time, the `rcu_head` pattern. Note nothing in this API names epochs:
A3- or A2-shaped engines could back a `Domain` later without consumer changes
(R8).

### 5.6 Safety argument (sketch, to be formalized in the spec)

The two invariants:

- **I1 (advance gate):** the epoch advances `e → e+1` only after a SEQ_CST
  scan observes every active slot's snapshot equal to `e`.
- **I2 (reclaim gate):** a bag tagged `e` is drained only when the global
  epoch is ≥ `e+2`.

Consider object O, unlinked from the structure and then retired at epoch `r`,
drained when the epoch reaches `r+2`, and any reader section S that
dereferences O. For S to hold a pointer to O, S's traversal must have loaded
it. Two cases against the SC order of the reader-activation fence and the
scan fences: (a) S's activation fence precedes the scan that justified some
advance toward `r+2` — then S was observed, and by I1 either S's snapshot
matched (so S exits before any *further* advance can satisfy I1 again, and
two advances separate retire from drain) or the advance was blocked until S
exited; (b) S's activation follows that scan — then S's post-fence loads are
SC-ordered after the unlink that preceded the retire, so S reads the
*unlinked* state and never loads O's address. Either way no section holds O
at drain time. Snapshot staleness (arbitrary delay between epoch load and
activation) only ever *blocks* advancement via the exact-match check — it is
a liveness cost, never a safety hole.

This argument's brevity is the decisive advantage over A3 and the reason A4
is recommended: it is short enough to formalize (litmus tests for the
fence-pairing core, §7) and to keep correct under maintenance.

### 5.7 Interaction with VMSubstrate/vmsmalloc — the CroCOS-specific part

- **Freshness is layered on top, not replaced** (§2). The tree stores child
  links so that traversal dereferences go through `SafePtr`-equivalent
  freshness (`ensureTLBEntryFresh` before first content read on this CPU).
  `ensureTLBEntryFresh` is loop-free and non-blocking — legal inside a
  read-side section.
- **Deleters are `destroy<T>` at the bottom.** The veneer provides a
  convenience `retireDestroy<T>` whose deleter poisons (debug) and calls
  `VMSubstrate::destroy<T>`. Grace-period expiry is precisely the point at
  which DEC-026's `vmsfree` validation chain may safely run.
- **RCU slot arrays live in per-domain static buffers** — always-mapped,
  never-recycled VA (no freshness concern for the framework's own state; the
  dirty-bit mechanism applies to remapped pages, and these are never
  remapped).
- **Immortal slabs are the safety net, not the mechanism** (A7 verdict): an
  escaped UAGP bug degrades to reading recycled typed memory — loud under
  the torture suite's poison discipline, never a wild fault.

## 6. Scheduler, timer, and IPI integration (additive only — R8, C5)

All of these accelerate *reclamation latency*; none are needed for safety.
The C5 discipline applies to every IPI mentioned here: fire-and-forget, no
waiting for delivery, and the system is fully correct (just slower to
reclaim) if the IPI never arrives.

First, a clarification of what a nudge IPI *can* target in this design.
EBR has no per-CPU counter that passively lags: an **inactive** CPU is
invisible to grace periods (nothing to refresh), and an **active** reader's
epoch snapshot is pinned by design until it exits its section — that pin *is*
the protection, so no nudge may touch it. The only nudgeable laggard state is
therefore (a) a CPU sitting on expired-but-undrained limbo bags, and (b) a
quiet system where nobody is running `tryAdvance`. Both are reclamation
latency, exactly where C5 permits IPIs.

- **Async drain-nudge IPI (likely the IPI subsystem's first real consumer;
  Phase R4/R5 era).** Covers the pathological quiet case: CPU X retired a
  pile of nodes, the epoch has since advanced past expiry, but X has gone
  idle and nothing will run its `drainLocal`. Any CPU that notices (e.g. a
  `tryAdvance` caller observing X's nonempty expired bags via the slot array)
  sends X a fire-and-forget nudge. One wrinkle: the handler must NOT drain in
  interrupt context (deleters bottom out in `vmsfree`; DEC-014). Instead it
  sets a CPU-local `drainPending` flag, and the wakeup itself pops X out of
  `hlt`, after which X's idle/return path runs `drainLocal` in normal
  context (the hook's exact home is Q7). A lost nudge means X's memory stays
  retired until the next natural trigger — bounded, and never a safety issue.
  The same handler shape later serves "please run `tryAdvance`" nudges for
  case (b).
- **Context switch / idle entry (scheduler era):** call `drainLocal` +
  `tryAdvance` on the outgoing CPU — free quiescence-adjacent moments that
  reduce how often the drain nudge is needed at all.
- **Preemption integration:** `ReadGuard` gains preempt-disable/enable;
  the §5.4 placeholder asserts become real (the DEC-030 trajectory).
- **Timer wheel (once timers are routine):** an *armed-on-demand* drain timer
  as a belt-and-suspenders complement to the drain nudge. Until either
  exists, the bound is explicit and acceptable: unreclaimed memory ≤ what was
  retired since the last advance, and any subsequent retire anywhere resumes
  progress.
- **Expedited grace periods (deferred, and in tension with C5).** The
  asymmetric-fence mode (demote the reader-side SEQ_CST fence to a compiler
  barrier; the GP side issues an IPI-broadcast barrier — the `sys_membarrier`
  trick) *can* be shaped to never block: the GP driver stamps a barrier
  generation, broadcasts fire-and-forget IPIs whose handlers fence and bump
  per-CPU ack counters, and only a *later* `tryAdvance` call that observes
  all acks for that generation trusts the scan — poll-based, no spinning on
  delivery. But unlike the drain nudge, this makes IPI *delivery* load-
  bearing for grace-period progress on every advance — heavy reliance, which
  C5 rules out as a default. It stays deferred unless the reader fence
  actually shows up in RadixVM-era profiles, and even then ships as an
  opt-in per-domain mode behind the named ordering constants. The userspace
  harness on macOS has no membarrier equivalent, so the fence-based protocol
  remains the tested baseline indefinitely.

## 7. Testing strategy

Per `[[project_armv8_dev_tsan]]`: ARMv8 M1 TSan is the primary release gate;
ASan/leak runners ship in parallel; subagent/CI claims re-verified by running
the suites.

- **`tests/kernel/rcu/`** following the vmsmalloc Phase 8 layout: `mocks/`
  reusing `bindThreadToCpu` + thread_local CpuLocal base + heap slot storage;
  real kernel headers under `CROCOS_TESTING`; `DebugIntrospection.h` exposing
  epoch/slot/bag internals behind a test-harness ifdef.
- **Unit tests (deterministic):** epoch advance blocked by exact-match
  mismatch; nesting (including simulated interrupt-nested sections); bag
  tagging/rotation/expiry algebra; `synchronize` advancing exactly two;
  retire-from-inside-own-section; stale-snapshot injection via a test hook
  that stalls a thread between epoch load and activation store, asserting
  advancement blocks until it exits.
- **Torture suite (rcutorture-shaped, the centerpiece):** N reader threads
  loop sections traversing a writer-mutated structure of versioned cells;
  writer threads swap and retire; deleters poison (`0xDD`) and, in the ASan
  runner, actually `free` so any use-after-grace-period is a hard ASan trap —
  this is the strongest UAF oracle available and the reason heap-backed mocks
  matter. Phases include stutter (all readers pause → full drain must occur),
  reader/writer ratio sweeps, and forced-stall injection (a reader holds a
  section; assert reclamation stalls *and* the debug stall-warning fires —
  liveness diagnostics are tested, not just trusted).
- **Ordering validation:** TSan exercises the protocol but does not prove
  fence pairings on its own; the activation-fence/scan-fence core (§5.6 cases
  a/b) gets herd7 AArch64 litmus tests checked in beside the spec (OPEN: Q5 —
  litmus vs. a short manual proof appendix; recommendation is litmus, the
  protocol core is small enough).
- **In-kernel stress (Phase R4):** a `smp_bringup` stress in the Phase 9
  mold (`CROCOS_RCU_STRESS` build flag, mutually exclusive with the others):
  per-CPU readers/writers over a shared test structure with real vmsmalloc
  nodes, real `SafePtr` freshness, real interrupt-context asserts — the
  validator for everything the userspace harness mocks.

## 8. Phasing

- **R1 — Core engine.** `Core::rcu::{EpochDomain, ReaderSlot, RetireHead,
  LimboBag}` + unit tests + ASan/TSan runners. No kernel dependencies.
- **R2 — Kernel veneer.** `kernel::rcu::Domain` over NUMA-placed slot
  buffers; `ReadGuard`; retire/`destroy<T>` integration; DEC-014-aligned
  debug asserts; harness integration tests via mock CpuLocal.
- **R3 — Torture suite.** The §7 suite; release gate for everything after.
- **R4 — In-kernel stress.** As above; gates RadixVM's dependency on the
  framework.
- **R5 — Consumer integration (with RadixVM).** Thresholds (`kBagCount`,
  drain trigger counts) tuned against real tree workloads — the Phase 10
  lesson: don't tune before a real workload exists.
- **Deferred:** expedited/asymmetric-fence mode; scheduler/idle/timer hints;
  preemptible (A3-style) domains if sleepable sections are ever needed.

## 9. Open questions

- **Q1 — Veneer placement.** `kernel/include/rcu/` as its own small subsystem
  (recommended: it is not VMM-specific, and `interrupts/`/`timing/` set the
  precedent for small cross-cutting subsystems) vs. under `mem/` next to its
  first consumer.
- **Q2 — Slot storage shape.** Domain-owned per-slot array in per-domain
  static buffers (recommended: zero `CpuLocal` growth per domain, domains
  fully self-contained and test-constructible) vs. embedding slots for a
  fixed small number of domains in `CpuLocal` (one fewer indirection on the
  read path; costs CpuLocal layout churn per new domain).
- **Q3 — Active-flag encoding.** Bit 0 of the packed state word (recommended)
  vs. sentinel epoch value; cosmetic, settle in spec.
- **Q4 — `kBagCount` and drain thresholds.** 4 bags, threshold ~64 retires
  PROPOSED as placeholders; real values are an R5 question.
- **Q5 — Ordering-proof artifact.** herd7 litmus files vs. manual proof
  appendix (see §7).
- **Q6 — Should `protect<T>` return a `SafePtr<T>`?** Folding freshness into
  the RCU load (one call does acquire + `ensureTLBEntryFresh`) is attractive
  consumer-shaping, but couples the Core-adjacent API to VMSubstrate. Leaning
  yes *at the veneer layer only* (Core engine stays raw); needs an API
  sketch against actual radix-node link types before deciding.
- **Q7 — Where does the drain-nudge's non-interrupt-context hook live
  pre-scheduler?** The §6 nudge handler can only set `drainPending` and wake
  the CPU; something in normal context must notice the flag. Pre-scheduler
  the natural home is the idle/`hlt` loop; the hook's shape (and what the
  framework asks of the IPI subsystem: one vector + a fire-and-forget
  `sendNudge(cpu)` with no delivery guarantee) should be co-designed with
  the IPI subsystem build-out, for which RCU is the motivating first
  consumer.

## 10. Summary

An epoch-based reclamation engine — per-CPU activation slots, a 64-bit
per-domain epoch, exact-match advancement, epoch-tagged per-CPU limbo bags,
pull-based progress driven from the retire path — wrapped in an
engine-agnostic `Domain`/`ReadGuard`/`retire` API. Correctness depends only
on explicit section entry/exit and `Core::Atomic` fences: no tick, no
scheduler, no IPIs on any correctness path — IPIs appear only as optional
fire-and-forget reclamation nudges (C1/C2/C5). The engine is a pure algorithm
torture-tested in
the existing userspace harness under ARMv8 TSan with ASan as a
use-after-grace-period oracle (C3). Idle CPUs are invisible to grace periods
(R3); the radix tree gets the simplest possible reader contract — acquire,
walk with `SafePtr` freshness, no revalidation, no ABA within a section (R5)
— and the scheduler, when it arrives, plugs in as a reclamation-latency
optimization, never a correctness dependency (R8).
