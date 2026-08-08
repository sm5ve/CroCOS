# vmsmalloc eager-free stale-TLB corruption — diagnosis findings

**Status:** Root-cause mechanism **identified and proven**. No fix applied yet.
**Date:** 2026-05-31.
**Tree state at handoff:** all diagnostic probes removed; tracked implementation
files (`kernel/mm/VMSubstrate.cpp`, `kernel/mm/vmsmalloc.cpp`,
`kernel/include/mem/VMSubstrate.h`, `libraries/Core/include/core/atomic/TreiberStack.h`)
reverted to **HEAD** (i.e. the original `freePage`-based eager-free, no DEC-047).
Kernel builds clean.

> ⚠️ Context note: this investigation started from the DEC-047 "read-only
> sentinel reclaim" work. That turned out to be a **red herring** for this bug
> (see below). The DEC-047 changes have been reverted from the tracked impl
> files, **but two leftovers remain** and should be reconciled when we resume:
> - `specs/vmsmalloc.md` still contains the DEC-047 row (2 mentions).
> - `tests/kernel/vmsmalloc/mocks/` still has the `reclaimSlabPage` mock stub.
> Neither affects the diagnosis or the HEAD kernel build.

---

## Symptom

`kernel/VmsmallocStress.cpp` (the Phase 9 in-kernel stress test, currently
untracked) detects content corruption: a slot it allocated and stamped with a
`(cpu, class, index, iteration)` pattern reads back, during the **same
iteration's verify pass**, as some *other* allocation's content (or the
`0xCC` free-poison, or a slab descriptor's `0x5DAB...` magic).

Originally reported by the user on the NUMA config. Example:
```
CONTENT MISMATCH cpu=2 class=8 index=8 iter=23 word=0 ptr=0xffffff0080246000
  expected=571746180661271 got=6749591459296233920   (0x5DAB5DABDE5CC9C0 = slab magic)
```

## The reproduction is concurrency-gated (critical for anyone re-investigating)

- **Single-threaded TCG (default `qemu -smp 8`) does NOT reproduce it** — 0 / 90
  boots. QEMU's default round-robin TCG does not truly interleave vCPUs, so the
  race window never opens. **This is why early runs looked clean and misled the
  investigation.**
- **MTTCG reproduces it reliably — ~9-10 / 10 boots.** Each vCPU runs on its own
  host thread, giving real parallelism.

Reproduction command (note: `-accel tcg,thread=multi` must be passed as separate
argv tokens — putting the whole QEMU arg list in a shell variable collapses it
and QEMU silently rejects `-accel`, which *also* produced false "clean" runs):

```bash
cd cmake-build-debug
qemu-system-x86_64 -accel tcg,thread=multi -smp 8 -m 256M \
  -object memory-backend-ram,size=64M,id=m0 -object memory-backend-ram,size=64M,id=m1 \
  -object memory-backend-ram,size=64M,id=m2 -object memory-backend-ram,size=64M,id=m3 \
  -numa node,memdev=m0,cpus=0-1,nodeid=0 -numa node,memdev=m1,cpus=2-3,nodeid=1 \
  -numa node,memdev=m2,cpus=4-5,nodeid=2 -numa node,memdev=m3,cpus=6-7,nodeid=3 \
  -cpu qemu64,+fsgsbase -no-reboot -nographic -kernel kernel/Kernel
```
(4 CPU-bearing domains maximize cross-domain free traffic; even 1-domain MTTCG
likely reproduces, but this was the reliable config used.)

## The bug is pre-existing at HEAD, independent of DEC-047

Reverting the 4 impl files to HEAD (eager-free uses plain `VMSubstrate::freePage`)
and running under MTTCG still reproduces **11 / 15**. So the corruption is a
property of the **HEAD eager-free reclaim path**, not something DEC-047
introduced. DEC-047's sentinel changed the *manifestation* earlier (it removed a
pagefault) but is not the cause.

## What was ruled OUT (each by a dedicated instrumented run under MTTCG)

1. **VA double-issue at `allocPage`.** Probe captured the prior leaf-PTE of every
   VA `allocPage` hands out. Across 8/10 corrupting runs it fired **zero** times —
   the prior PTE is always non-present. The radix VA allocator is *not* handing
   out a VA whose mapping is still live.
2. **Physical-frame double-issue.** A 64 KiB per-frame ownership shadow
   (one atomic byte per 4 KiB frame) in `allocPage`/`freePage`: mark-owned on
   alloc (assert was-free), mark-free on free (assert was-owned). **Zero**
   double-alloc and **zero** free-of-unowned across 9/10 corrupting runs. The
   page allocator's accounting is correct.
3. **Reclaim of a slab with live slots.** Probe re-read `bookkeeper.allocatedSlotCount()`
   at the instant of `freePage(m.head)` in the eager-free walk. **Never** non-zero —
   `isEmpty()` → `freePage` only ever reclaims genuinely-empty slabs.
4. **Allocator returning overlapping virtual address ranges.** Probe in the stress
   test scanned every fresh allocation against all still-live pointers for VA
   overlap. **Zero** overlaps. The two colliding allocations live at genuinely
   distinct VAs.

## What was CONFIRMED — the actual mechanism

**The eager-free walk is the trigger.** Disabling just the DEC-036 eager-free
loop (so empty slab pages are never `freePage`'d) drops corruption to **0 / 15**
under MTTCG (vs 9-10/10 with it enabled).

**The mechanism is a stale TLB / stale VA→PA translation on a CPU that holds a
`vmsmalloc` pointer.** Proven two ways:

- At the mismatch, resolving the victim VA's *current* leaf PTE
  (`diagResolvePhys`) showed cases where the PTE is **non-present** (`victimPhys=0`)
  yet the read still returned data — i.e. the read was served from a **stale TLB
  entry** for a VA whose page-table mapping had already been torn down. Other
  cases showed the victim VA resolving to a frame now holding a *different*
  allocation's content.
- Adding `VMSubstrate::ensureTLBEntryFresh(p)` immediately before the holder's
  read (in the stress test's `verifyPattern`) **eliminates the corruption:
  0 / 15** under MTTCG.

### Why it happens (the chain)

1. The DEC-036 eager-free walk calls `VMSubstrate::freePage(slabPage)` on an
   empty slab. `freePage` clears the leaf PTE, calls `setDirtyForOtherCPUs(va)`
   (marks the VA dirty in *other* CPUs' dirty-bitmaps so they will lazily
   `invlpg` on next `ensureTLBEntryFresh`), does a local `invlpg`, and returns
   the physical frame to `PageAllocator`.
2. That physical frame is promptly re-allocated (by `allocPage` — a whole-page
   alloc, or a fresh slab) and mapped at a **different VA**, with fresh content.
3. A CPU that still holds a pointer into the **old** VA (e.g. another live
   allocation in the stress test's batch, or — in the original report — the
   Treiber/magazine machinery) reads through its **stale TLB entry** for the old
   VA, which still points at the recycled physical frame. It therefore observes
   the *new* allocation's bytes (or `0xCC` poison, or slab magic).

The crux: VMSubstrate's cross-CPU TLB-coherence is **lazy and pull-based** —
`freePage`/`allocPage` only *flag* other CPUs via the dirty bitmap; a remote CPU
only actually invalidates its stale TLB entry when it calls
`ensureTLBEntryFresh` before a read. The slab allocator does this religiously on
Treiber-stack heads (the `onPreTouch` hook, DEC-040) and at magazine `m.head`
transitions. **But ordinary holders of a `vmsmalloc`-returned pointer never call
`ensureTLBEntryFresh` before dereferencing it** — and nothing forces the freed
frame to stay un-recycled until every CPU's stale TLB entry is gone.

So any VA whose backing slab page is `freePage`'d by eager-free while another CPU
has a live TLB entry for an address on that page is a stale-translation hazard
the moment that physical frame is recycled.

> Note: `victimPhys=0` (PTE torn down, read served from stale TLB) appeared even
> for the verifying CPU's *own* held pointers. The exact cross-CPU vs same-CPU
> attribution wasn't fully pinned (the dirty-bitmap protocol means the *freeing*
> CPU is excluded from its own `setDirtyForOtherCPUs`, so a same-CPU stale entry
> is plausible if the freeing CPU and the holding CPU differ, which they do under
> the handoff path). The decisive facts are: eager-free is the trigger, and a
> freshness call on the read side fixes it.

## Key code locations

- Eager-free walk (trigger): `kernel/mm/vmsmalloc.cpp` ~line 524, the
  `while (m.depth > 1 && ... isEmpty())` loop calling `VMSubstrate::freePage(m.head)`.
- `freePage` / `setDirtyForOtherCPUs` / `ensureTLBEntryFresh`:
  `kernel/mm/VMSubstrate.cpp` (`freePage` ~827, `setDirtyForOtherCPUs` ~408,
  `ensureTLBEntryFresh` ~1015 region).
- Dirty-bitmap freshness protocol: `ensureTLBEntryFresh` checks *this* CPU's
  dirty bit for the VA's slot and `invlpg`s if set; `setDirtyForOtherCPUs` sets
  the bit for all CPUs except the caller.
- Whole-page free path: `vmsfree` page-aligned bypass, `kernel/mm/vmsmalloc.cpp` ~586.

## Implications for the fix (for discussion next session — NOT yet decided)

The hazard is fundamental to lazy/pull-based TLB coherence + physical-frame
recycling: **`freePage` returns a frame to the allocator before it is guaranteed
that no CPU holds a stale TLB entry mapping that frame.** Candidate directions
(to weigh together, no recommendation yet):

- **Don't recycle the physical frame until quiescent** (epoch/quiescence before
  `PageAllocator::freeSmallPage`), so a stale TLB entry only ever points at the
  *same* logical page.
- **Eager cross-CPU TLB shootdown** on `freePage` of a slab page (IPI) instead of
  the lazy dirty-bitmap — correct but costly.
- **Stop eager-freeing slab pages** (pure DEC-002 lazy reclaim); empty slabs stay
  mapped, frame never recycled out from under a holder. Removes the trigger
  entirely at a memory-retention cost.
- **DEC-047 sentinel reclaim** keeps the VA mapped (read-only) so a stale read is
  harmless — but note the earlier finding that the sentinel variant which *also*
  released the VA still corrupted; any sentinel approach must keep the frame from
  being recycled into a *different* live allocation. This needs more thought given
  what we now know.

The earlier DEC-047 "pin the VA" experiment that looked 9/9 clean was **never
actually built** (the edits were cancelled by a classifier outage mid-run), so
that result is invalid and must be re-tested if revisited.

## How to reproduce the diagnostic experiments

All probes have been removed, but they were: (a) prior-PTE classifier in
`allocPage`; (b) per-frame ownership shadow in `allocPage`/`freePage`;
(c) `allocatedSlotCount()` re-check in the eager-free walk; (d) VA-overlap scan
in the stress test alloc loop; (e) `diagResolvePhys` + victimPhys in the mismatch
report; (f) `ensureTLBEntryFresh(p)` in `verifyPattern` (the fix-confirming one);
(g) `while(false && ...)` to disable eager-free. Re-add as needed; remember to
`#include <kernel.h>` in `VMSubstrate.cpp` for `klog`, and build under MTTCG.
