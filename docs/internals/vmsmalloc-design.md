# vmsmalloc / vmsfree Design — Open Questions and Task Backlog

This document is the working design record for the sub-page slab allocator
that will sit on top of `VMSubstrate::allocPage` / `freePage`. The
declarations `VMSubstrate::vmsmalloc` and `VMSubstrate::vmsfree` already
exist in `kernel/include/mem/VMSubstrate.h` but have no bodies; this doc
tracks what needs to be decided and built to fill them in.

It is intended to be **edited in place** as decisions are made — each
open question carries a `**Status:** Open | Decided <date>` line plus a
short rationale once decided. Implementation work links back to specific
question IDs.

## Confirmed prerequisites (already in tree)

- `LibAlloc::SlabBookkeeper` and `LibAlloc::Slab` — bitmap-backed slot
  tracker with `OccupancyTransition` reporting; full-fat layer with
  caller-provided backing memory (`libraries/LibAlloc/include/liballoc/Slab.h`).
- `Core::SplitBitmap` — single-producer / many-consumer concurrent
  bitmap with inline or external storage
  (`libraries/Core/include/core/atomic/SplitBitmap.h`).
- `VMSubstrate::allocPage` / `freePage` — per-CPU arenas, multi-CPU-free
  safe page allocator (`kernel/mm/VMSubstrate.cpp`).
- `NUMAPool::paPages` (an `AtomicBitPool`) — working reference design
  for a partial-pool data structure with multi-reader scaling
  (`kernel/include/mem/PageAllocator.h`).
- `LocalPool[arch::MAX_PROCESSOR_COUNT]` — the kernel's existing per-CPU
  storage pattern (static array keyed by `ProcessorID`).
- `BigPageMetadata::allocHolder` (`Atomic<size_t>`) — the existing
  pattern for "single CPU at a time alloc-side ownership" with
  `markAllocHolder` / `releaseAllocHolder`.

## Open questions

Each `Q#` is a self-contained decision. The dependency arrows in the
task backlog at the bottom reference these IDs.

### Geometry & size classes

#### Q1. Size-class table
Power-of-2 only (8, 16, 32, …, 2048), or include intermediates
(12, 24, 48, …) for better internal-fragmentation behavior?
**Status:** Open.
**Options:** strict pow2 (simple, easy log-shift lookup) · pow2 + 1.5×
intermediates (jemalloc-style; fewer wasted bytes) · custom hand-tuned
list.
**Recommendation:** strict pow2 for v1; revisit if workload measurements
show worth doing.

#### Q2. Pages per slab per size class
Small classes (8 B) get many slots from one page; large classes
(1–2 KB) only get 4 slots from one page. Use multi-page slabs to
equalize slot counts?
**Status:** Open.
**Options:** always 1 small page · multi-page slabs for large classes
to hit a minimum slot count · per-class fixed table.
**Recommendation:** per-class table that targets ≥ 64 slots per slab
(one `SplitBitmap` word) for each class.

#### Q3. Slot count rounding
`SlabBookkeeper` requires `SlotCount` to be a multiple of 64. With a
non-divisible page-bytes-to-slot-size ratio, do we round slot count
down (waste bytes) or pad to the next 64 and pre-reserve the tail?
**Status:** Open.
**Recommendation:** pick per-class page counts so slot count is
naturally a multiple of 64; this falls out of Q2 cleanly.

#### Q4. Maximum vmsmalloc-managed size
Largest size that goes through the slab layer before vmsmalloc
falls back to `allocPage` directly?
**Status:** Open.
**Options:** `smallPageSize / 2` (2 KB), `smallPageSize` (4 KB), or
larger (multi-page slabs).
**Recommendation:** 2 KB cap for v1 — anything ≥ 4 KB is at minimum one
whole page, just return `allocPage` directly.

### Slab metadata

#### Q5. Where does per-slab metadata live?
**Status:** Open.
**Options:**
1. **Header at slab base** — slot 0 reserved for metadata. Easy
   `ptr & ~(slabSize-1)` lookup, but wastes a slot and constrains
   metadata size.
2. **Parallel metadata array** keyed by slab index in the arena. No
   wasted slots, but a side table.
3. **Dedicated metadata pool** allocated separately at boot from a
   bootstrap arena. Cleanest, but adds a sub-allocator.
**Recommendation:** start with option 1 if metadata fits in one slot
of the smallest class, otherwise option 2 keyed by
`(slabBase - arenaBase) / slabSize`.

#### Q6. Metadata backing-memory source
`vmsmalloc` cannot bootstrap its own metadata (chicken-and-egg).
**Status:** Open.
**Options:** `BootstrapAllocator` at init · a dedicated arena from
`allocPage` reserved at boot · piggyback on the radix-tree's
occupancy buffer.
**Recommendation:** dedicated arena of `allocPage`-allocated pages,
managed by a tiny bump-or-Slab allocator. Avoids polluting bootstrap.

### Address → slab lookup on `vmsfree(ptr)`

#### Q7. Owning-slab lookup strategy
**Status:** Open.
**Options:**
1. **Alignment trick** — every slab is aligned to its full size;
   `ptr & ~(slabSize-1)` is the slab base. Metadata then via Q5.
2. **Radix-tree reverse map** — extend VMSubstrate's radix tree to
   also point at owning-slab metadata for non-empty leaves.
3. **Per-arena slab table** — `(ptr - arenaBase) / minSlabSize` indexes
   a side table.
4. **Pointer tagging** — encode size class in low/high bits. Rejected
   as too fragile.
**Recommendation:** option 1 paired with Q5 option 1 — slabs are
power-of-2 aligned (e.g. all slabs are page-aligned), header at slab
base carries size-class + bookkeeper pointer. This bounds slabs to
one or a small power-of-2 number of pages, which is fine given Q4.

### Cross-CPU free & partial-slab pool

#### Q8. Partial-slab pool data structure (deferred from Phase B)
**Status:** Open.
**Options:**
1. **AtomicBitPool** per NUMA domain — parallel to
   `NUMAPool::paPages`. NUMA-aware, multi-reader-scalable, but cost
   scales with slab count.
2. **Treiber stack** per CPU or per NUMA domain — simpler, ABA-safe
   if slabs aren't reused mid-list. Cross-NUMA stealing needs
   explicit logic.
3. **Per-CPU bounded ring** like `LocalPool`'s 2-slab cache, with
   overflow spilling to a global structure.
**Recommendation:** mirror what works for big pages — per-CPU active
slab(s) plus per-NUMA-domain `AtomicBitPool` of partial slabs. The
upfront cost is bounded by the metadata arena size and we already
have working scaling characteristics on this shape from `paPages`.

#### Q9. Who republishes a slab on `becameAvailable`?
A remote-CPU free transitions a slab from Full → Partial. Some entity
must add it back to the partial-slab pool.
**Status:** Open.
**Options:** freer republishes inline · freer sets a flag, next
allocator probe republishes · slab carries an "in-pool" bit and the
freer CAS-publishes if absent.
**Recommendation:** the third option — same race-free pattern
`NUMAPool::returnPage` uses for big pages.

#### Q10. Active-slab ownership model
Single owning CPU per slab (so non-atomic alloc-side bitmap stays
non-atomic, like `BigPageMetadata::allocHolder`), or multi-CPU alloc
from one slab?
**Status:** Open.
**Recommendation:** single-owner, mirroring `BigPageMetadata`. The
`SlabBookkeeper` is already designed for that model — going multi-owner
would force the alloc bitmap to become atomic and degrade hot-path
perf.

### Slab lifecycle

#### Q11. Empty-slab disposition (Partial → Empty)
**Status:** Open.
**Options:** immediate `freePage` · cache 1 empty per class per CPU
(hysteresis) · global bounded cache.
**Recommendation:** 1-per-class-per-CPU hysteresis. Tight alloc/free
loops at one size won't thrash; memory return is still bounded.

#### Q12. Steal threshold for cross-CPU / cross-NUMA fetch
When local active and per-CPU partial cache are dry, where do we
look next?
**Status:** Open.
**Recommendation:** same NUMA domain via the domain's partial pool
(Q8) first, then cross-domain via NUMA fallback order — same policy
the page allocator uses.

### Stack integration

#### Q13. What does vmsmalloc replace?
Today: `kmalloc → la_malloc → static heap_buffer`. Options:
**Status:** Open.
1. **vmsmalloc IS the LibAlloc backend** —
   `LibAlloc::Backend::allocPages` calls `VMSubstrate::allocPage`,
   slab layer lives inside LibAlloc.
2. **kmalloc routes directly to vmsmalloc** — bypass LibAlloc
   entirely for kernel-side allocation.
3. **vmsmalloc replaces la_malloc** — the rewrite of
   `LibAlloc::SlabAllocator` is built on top of `LibAlloc::Slab` and
   kmalloc routes through that.
**Recommendation:** option 2 for the kernel — vmsmalloc lives in
`kernel/mm/` and kmalloc calls it directly. LibAlloc's eventual
rewrite is a parallel effort for userspace consumers.

#### Q14. Alignment contract for `vmsmalloc(size)`
**Status:** Open.
**Options:** natural-alignment (≥ alignof(max_align_t)) · always
size-aligned · cache-line for size ≥ 64.
**Recommendation:** size-aligned for power-of-2 size classes (falls
out for free from slot layout); add an explicit
`vmsmalloc(size, alignment)` overload if a caller needs more.

#### Q15. OOM behavior
**Status:** Open.
**Recommendation:** panic, matching `allocPage`'s current contract.
Add a `try_vmsmalloc` variant later if any caller needs softer
semantics.

### Loose ends from prior phases

#### Q16. Re-namespace `OccupancyTransition` under `LibAlloc::`?
Deferred this round.
**Status:** Open.
**Recommendation:** do it before vmsmalloc lands so the new layer
doesn't entrench `Core::` in fresh code. Small change, ~4 callsite
updates.

#### Q17. Backing-allocator concept for full-fat `Slab`?
Today `Slab` takes `void* base` from a caller. For vmsmalloc the
slab pool needs to grow itself.
**Status:** Open.
**Options:** extend `Slab` with a backing-allocator template
parameter · keep `Slab` as-is and let the slab-pool layer own page
acquisition.
**Recommendation:** keep `Slab` as-is. The pool layer is where
size-class and lifecycle logic lives anyway; coupling page
acquisition into `Slab` would mix concerns.

## Task backlog

Rough dependency order; each task tags the questions it depends on.

| # | Task | Depends on |
|---|------|------------|
| A | Pick size-class table + slab geometry | Q1, Q2, Q3, Q4 |
| B | Pick metadata storage + backing source | Q5, Q6 |
| C | Pick address→slab lookup strategy | Q5, Q7, A |
| D | Pick partial-pool data structure (possibly via small prototype/bench) | Q8 |
| E | Pick stack-integration shape | Q13 |
| F | Re-namespace OccupancyTransition (if Q16 = yes) | Q16 |
| G | Implement `SlabPool<SizeClass>`: per-CPU active slab + partial pool + empty cache + republish-on-`becameAvailable` / retire-on-`becameEmpty` lifecycle | A, B, C, D, Q9, Q10, Q11 |
| H | Implement `vmsmalloc(size)` — size-class lookup → `SlabPool::alloc`, oversize → `allocPage` | G, Q4, Q14, Q15 |
| I | Implement `vmsfree(ptr)` — address → slab → `SlabPool::free`; cross-CPU + republishing | G, C, Q9 |
| J | Unit tests in `tests/kernel/`: per-size-class round-trips, slot uniqueness, Empty/Full transitions, cross-CPU free, oversize fallback | H, I |
| K | Extend stress test from commit 061cf55 to exercise vmsmalloc concurrently with VMSubstrate's pool | H, I |
| L | Wire into kmalloc/LibAlloc per E; remove `KERNEL_INIT_HEAP_BUFFER` if no longer needed | H, I, E |
| M | Add `CROCOS_TESTING` accessors for per-size-class allocated/peak/slab counts | G |
| N | Update this doc with all decisions; close out open questions | all |

## Verification plan

When the implementation reaches L:

1. `cmake --build cmake-build-debug --target unit_tests` green, including
   new vmsmalloc cases.
2. `cmake --build cmake-build-debug --target run` and `… run_numa_hmat`
   boot cleanly. NUMA-HMAT variant exercises the cross-domain partial
   pool logic.
3. The stress test from K runs for ≥ 30 s without panics or leaks.
4. Address-sanitizer + leak-sanitizer unit-test runs clean.

## Decision log

Decisions land here once an open question is resolved, with date and
short rationale. Newest first.

_(empty)_
