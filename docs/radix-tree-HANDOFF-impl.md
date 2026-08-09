# radix-tree — implementation handoff

**Written 2026-08-08 at `be2681c`.** Branch `radix-tree`, local-only, unmerged
(16 commits ahead of `1d448e0`). **Phases 0–4 complete and green; Phase 5 partly
landed.** This session went well beyond the radix tree — it changed `SafePtr`,
the kernel's logging rules, and added a pinned-memory allocator — so read §0 and
§1 before anything else.

Read in this order:

1. **§0** — state, and the two things waiting on Spencer.
2. **§1** — what changed outside the radix tree.
3. **§2** — the next work item, fully specified (the RCU reader-slot block).
4. `docs/radix-tree-implementation-deviations.md` — D-001..D-047. **Live:
   D-004, D-010, D-029, D-030 (older); D-033/034/036/039/042/044/046 owe the
   spec text.**
5. `specs/radix-tree-phase-5.md` — the remaining phase work (calibration).
6. §3 for traps, §0.1 onwards for background.

---

## 0. State

### Green

| Phase | State |
|---|---|
| 0–3 | Complete. See §0.1 and §4 below. |
| **4** | **Complete.** DEC-078 seam (`PinnedNode`, `pinLocked`, `resumeDescent`, `release`) and DEC-079's cache: per-CPU direct-mapped set of 4, VA-range-deferred install, generation check, `Detached`/generation eviction. All four §13 gate properties tested; the calibration report runs in the suite. |
| **5** | **Partly landed.** Kernel codec instance, node/`Mapping` censuses, the in-kernel stress, and the whole freshness family. Debug **and** Release/LTO boot clean on `run`, `run_numa`, `run_numa_hmat`; Release reaches 1025 cycles/boot, 8-of-8. **Calibration untouched.** |

Full suite, sequential: Core 441 + 425 TSan, Kernel 177, LibAlloc 38×2, vmsmalloc
31×2, RCU 61×2, **radix 156×3** (ASan, TSan, progress audit). The default kernel
(stress off) builds and boots.

```
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DCROCOS_RADIX_STRESS=ON
cmake --build cmake-build-debug --target run        # or run_numa / run_numa_hmat
```

`CROCOS_RADIX_STRESS` **selects** the smp_bringup stress rather than adding one —
it takes RCU Phase 4's slot by tail-call from `rcuStress`, because two per-CPU
routines in one phase means the first one's infinite loop silently starves the
second, which looks exactly like a healthy boot. `RadixStress.cpp` compiles
either way on purpose: it is the only TU that type-checks the tree against the
real freestanding toolchain and `-Werror`.

`-DCROCOS_RADIX_STRESS_OPS=24` turns a debug boot's 4 cycles into 1025 with every
assert live. **That is the most useful debugging lever here** — it is how both of
Phase 5's defects became reproducible.

### The two things waiting on Spencer

Design questions, not bugs; options are in the deviations log.

1. **The root bucket page** (D-039). The only per-address-space structure whose
   *mapping* churns — `tryMake` at creation, `freePage` at teardown, VA re-backed
   for something else — and that churn produced a real stale-read bug. Folding it
   into the control block deletes the hazard at source instead of paying a
   per-access freshness check on the tree's hottest read. Cost: +4 KiB against a
   768 B block, so it becomes 84% of the reservation. **Decide against current
   numbers, not the ones in D-045's first table.**
2. **Per-level node freshness** (D-042). One `ensureTLBEntryFresh` per level on
   the descent. Keep and price it / nodes from never-recycling storage (DEC-082's
   answer one level down) / a stronger vmsmalloc guarantee. Needs a cache-miss
   measurement, not `-icount`: the instruction count is small and the extra cache
   line per node is not.

### Phase 5's remaining work

- **Calibration, entirely untouched**: `detachBudget`, `drainBatchBound`,
  ITEM-084's draw-count histogram, ITEM-055's state-word placement, DEC-095's K
  and `scanChunk`, DEC-079's entry count. `-icount` per RCU P4-DEC-010, **not**
  TCG block counts. D-034 records what the Phase 4 harness could and could not
  settle.
- **Finish the freshness audit deliberately** rather than by defect: walk §7.1's
  list against the code and record each site as covered or exempt. Six were found
  the hard way (§1.1).

---

## 1. What changed outside the radix tree

### 1.1 The freshness family — six sites, four of them unlisted

**The in-kernel stress found a real freshness defect on its first boot** — the
vmsmalloc DEC-047 precedent repeating exactly: a stale-TLB bug class the
userspace harness is *structurally* incapable of seeing, because its
`ensureTLBEntryFresh` is a no-op and it has no page tables. 150 tests on two
sanitizers passed with every one of these calls absent.

| Site | Named by §7.1? | Fixed in |
|---|---|---|
| Node deleter's slot reads + own refcount RMW | yes | `ae23572` |
| Either deleter RMW-ing a `Mapping`'s count word | yes | `ae23572` |
| The root bucket page (read by EVERY descent) | **no** | `ae23572` |
| Reader's/writer's first touch of a `Mapping` body | **no** | `ae23572` |
| A node pointer decoded from a slot word | **no** | `06a3d8f` |
| **A `Mapping` accessed on a CPU that did not look it up** | **no** | `a2fc4e5` |

The last row was *wrong*, not merely unidiomatic, and it was the residual page
fault: a freshness call made once at acquisition guarantees nothing to a CPU that
did not make it, and DEC-015 exists precisely so a `LookupResult` can close its
section, block on a pager, and resume on another CPU.

**§7.1's list is incomplete in four places, and the reader-vs-deleter distinction
it implies is not sound.** Spec amendment owed (D-039, D-042, D-044).

### 1.2 `SafePtr` grew, and everything routes through it

Spencer's direction: *"the VMM is the only consumer of SafePtr, let's not
calcify the API."* Bare `ensureTLBEntryFresh` calls are gone from the radix
headers.

Added: `address()` (typed, discharges nothing — identity, encoding, and the
concrete-type cast `retire`/`destroy` need); `at<U>(byteOffset)` (a typed
sub-object made fresh on *its own* page); **`SafePtr<void>`** (type-erased, whose
motivating consumer is `NodeRef` — a descent stands on nodes of mixed valence and
cannot name a concrete `Node<G,V>`); hidden-friend comparisons so a raw pointer
works on either side; a default ctor and move assignment so `LookupResult` can
hold one.

`LookupResult` holds and returns `SafePtr<Mapping>`. The `...FromDeleter` split
collapsed — by the migration argument the reader's release sits exactly where the
deleter's does.

**Three copies of `SafePtr` exist** (real + radix mock + vmsmalloc mock) and must
be kept in sync. Collapsing them is worth doing.

### 1.3 `klog` is not safe from interrupt context

`AtomicPrintStream` holds a **global spinlock for the whole lifetime of the
temporary**, so `klog() << a << b;` is one critical section. An interrupt whose
handler logs on a CPU that is mid-statement re-acquires a lock it already holds.
Debug catches it (self-deadlock detector); **release compiles the detector out and
the same interleaving is a silent hard hang with the log lock held** — which the
hang watchdog cannot report, because the watchdog is itself a timer event on the
wedged CPU.

Six sites moved to `emergencyLog()`: `enqueueShutdown`'s `Goodbye :)`, smp.cpp's
"All N processors up!", both stress watchdogs' `reportHangAndExit`, and four lines
inside `dispatchInterrupt` itself. Documented at `klog()` in `kernel.h`, at
`emergencyLog()` in `panic.h`, and at `AtomicPrintStream`. `emergencyLog`'s useful
property is **lock-free**, not "for crashes" — the panic path was merely its first
consumer.

### 1.4 `PinnedBlockPool` — pinned memory got an allocator

`kernel/include/mem/PinnedBlockPool.h`. Fixed-stride, per-NUMA-domain intrusive
freelist over page-granular reservations, carving each page into as many blocks as
it holds. Spencer's framing: *"it can just be a big linked list of control blocks
with a lock."*

| | before | after pools fix | with the pool |
|---|---|---|---|
| radix block, window cost | 20,480 B | 4,096 B | **819 B** (5/page) |
| total per address space | 24,576 B | 8,192 B | **4,915 B** |
| ceiling (concurrent AS) | ~43,700 | ~131,000 | **~218,000** |

Three facts that are load-bearing and easy to get wrong:

- **Reservations round up to whole pages.** `staticBufferNextVA += pages *
  smallPageSize`. **Measure the window, not the request** — D-045 got this wrong
  and D-046 corrects it.
- **Reuse of a pinned block is free and needs no shootdown.** It touches no PTE:
  the VA→physical mapping is unchanged and only the bytes differ, so a CPU's
  cached translation stays *correct*. DEC-051b's write-once-PTE invariant buys
  that; it fights *returning pages to the allocator*, never reuse.
- **The pool never zeroes.** RCU-DEC-043(i) is the rule: zeroing at the consumer
  "makes every init valid regardless of block provenance, which is the only form
  of the rule that does not depend on how the caller obtained its storage."

The cross-domain fallback is the **last** resort, after carving. Writing it as the
second choice means a carve's spare blocks are handed to the first request for a
*second* domain, so the machine never carves on that domain at all — caught by a
test, recorded in D-047.

`numa::kMaxDomains` was **promoted**, not invented: it lived in vmsmalloc's
internal `VMSubstrateSlab.h`, and a cap on `DomainID` belongs with `DomainID` once
a second subsystem needs it. vmsmalloc keeps its spelling as an alias.

---

## 2. NEXT: the RCU reader-slot block

**Now the dominant per-address-space cost by 5×** — 4,096 B against the radix
block's 819 B — and the highest-value remaining item on this thread. Deliberately
not done, for reasons worth having up front.

### The situation

`Domain::init` → `acquireSlotBlockLocked` → `reserveFreshSlotBlockLocked`
(`kernel/rcu/RCU.cpp` ~276–350):

```cpp
const size_t pages = divideAndRoundUp(cpuCount, kSlotsPerPage);
for (size_t p = 0; p < pages; p++) {
    const auto firstCpu = static_cast<arch::ProcessorID>(p * kSlotsPerPage);
    const numa::DomainID d = numa::numaPolicy().homeDomain(firstCpu);
    void* got = tryReservePerDomainStaticBuffer(arch::smallPageSize, d);
    ...
}
```

`sizeof(ReaderSlot)` is **128 B**, so `kSlotsPerPage` is **32**. An 8-CPU machine
needs 1,024 B and takes a whole 4 KiB page — **75% wasted, on every address
space**. Blocks recycle through `gFreeSlotBlocks`
(`popFreeSlotBlockLocked`/`pushFreeSlotBlockLocked`), pushed by `Domain::deinit`.

### Already checked — do not redo

- **`gFreeSlotBlocks` does NOT have the NUMA bug radix's freelist had.** RCU
  derives placement from the fixed CPU topology (`homeDomain(p * kSlotsPerPage)`),
  so every block has identical per-page placement and blocks are interchangeable.
  Radix's differed only because `home` is a caller-supplied per-address-space
  parameter.
- **RCU already re-zeroes on reuse** (`memset(block, 0, blockBytes)` in
  `Domain::init`, RCU-DEC-043(i)) — so the pool's no-zeroing contract already
  suits it.
- **Reserving less saves nothing.** The bump pointer advances by whole pages, so a
  1,024 B request consumes the same 4 KiB. The saving exists only via sub-page
  packing, i.e. `PinnedBlockPool`.

### The two obstacles

1. **Per-page NUMA placement.** Page `p` is deliberately placed on the domain of
   the CPUs whose slots it holds. A fixed-stride pool hands out blocks, not
   page-aligned runs with per-page placement.
2. **A contiguity dependency.** `reserveFreshSlotBlockLocked` asserts consecutive
   reservations return consecutive VAs, because the slot array must be dense — and
   says so: *"CONTIGUITY DEPENDENCY: consecutive reservations returning consecutive
   VAs is a property of VMSubstrate's bump allocator, not a documented contract."*
   A shared pool **breaks that** by interleaving carves from other consumers.

### The shape of the answer

**The single-page case has neither problem, and it is every machine up to 32
CPUs** — the entire consumer-desktop target. For `cpuCount <= kSlotsPerPage`
there is one page (so per-page placement already degrades to "pick one domain" —
it picks `homeDomain(cpu 0)`) and one reservation (so contiguity is vacuous).

A `PinnedBlockPool` with stride `cpuCount * sizeof(ReaderSlot)` serves it
directly: 1,024 B → **4 slot blocks per page**, taking the per-address-space cost
from 4,915 B to **~2,048 B** and the ceiling from ~218,000 to **~520,000**.

Suggested shape, leaving the multi-page path untouched:

```
cpuCount <= kSlotsPerPage  -> PinnedBlockPool, stride cpuCount*sizeof(ReaderSlot)
otherwise                  -> the existing page-per-group path, unchanged
```

The branch is honest rather than ugly: the cases genuinely differ in whether
per-page placement means anything. Put the `if` where the reason is visible and
say why.

**Watch for**: `Domain::deinit` must return the block to whichever source gave it
out, so `slotBlock` alone is no longer enough. `PinnedBlock` carries the domain
for exactly this reason — storing one in `Domain` beside `slotBlock` is the
obvious move.

**Do not** simply retire the multi-page path: it exists for >32-CPU machines and
its per-page placement is real. Unifying would need a contiguous-extent request
from the pool, which is a bigger change than this item requires.

### Gate

RCU's own suite (`KernelRcuIntegrationTestRunner{,TSan}`, 61×2) plus the radix
stress, whose ~1025 domain create/destroy pairs per boot is the only thing that
exercises this path at volume. Add a packing test mirroring
`radix_address_space_store_packs_blocks_into_a_page`, and **mutation-test it** — a
packing test that passes vacuously tells you nothing.

---

## 3. Traps worth not rediscovering

- **`-DCROCOS_RADIX_STRESS_OPS=24`** turns 4 debug cycles into 1025 with asserts
  live. Use it before investigating anything create/teardown shaped.
- **Release/LTO is ~250× more informative than debug** in the same 20 s window;
  both Phase 5 defects only ever appeared there. A hang shows as no `Goodbye :)`
  and the full timeout — grep the verdict line, not the exit code, since `timeout`
  masks the difference.
- **A single page does not make a deep tree** (D-038). At level 4 the range unit
  is already 4 KiB, so one mapping lands two nodes down. Fixtures wanting a
  floor-level node map a **pair** of adjacent pages.
- **Any count-checking validation must `quiesce(h)` first** — releases are
  deferred.
- **Every destroy path routes through `destroyNode`** so the census is one pair of
  counters, not a set that must agree.
- **D-029 stays a WATCH item** — do not re-silence a `Mapping`-ctor-vs-`offsetFor`
  race; the alternative explanation is a real use-after-free.
- **Mutation-test anything whose failure mode is passing vacuously.** It paid off
  four times this session, including catching that the pool's cross-domain
  fallback was written as the second choice rather than the last.
- Ranges are floor-unit granular; a misaligned `hi` trips "subdivision ran past
  the resolution floor".
- `CROCOS_SKIP_TSAN_STRESS` defaults ON — see the Core TSan starvation note.
- **Run cmake from `tests/`**, not the repo root, for the unit tests; the root
  `cmake-build-debug` is the kernel build. `run_all_tests` at the end of a phase
  (D-021).
- New radix headers must be added to `OrderingSpellingTest.cpp`'s header list, or
  their atomics are invisible to §11's spelling check.

---

## A. Historical: the Phase 3 handoff (superseded by §0–§1, kept for background)

**All nine work items have landed.** Full suite green, sequential on a quiet
machine: Core 441 + 425 TSan, Kernel 177, LibAlloc 38x2, vmsmalloc 31x2, RCU
61x2, **radix 138x2** plus the progress audit. Kernel builds and boots clean.

| Commit | Item |
|---|---|
| `313a2ae` | `vmsmallocTry` / `tryMake` kernel-side (DEC-048). Closes D-003. |
| `8a5b1f7` | `DeferredRelease` pools (DEC-068). **Closes D-011.** |
| `4e6135b` | The validity token and `revalidate` (§3.1, DEC-051/070). |
| `9efb9a7` | Bucket table + packed entry codec (DEC-102/103). |
| `9e80cc2` | The root-binding seam (`currentBinding`). |
| `28e8c6c` | Cluster creation and growth (§5.6). |
| `11b4c54` | Chunked ordered enumeration (DEC-083). |
| `5fdb7cf` | Placement + the DEC-091 assignment seam. |
| `d7f822c` | Control block, creation sequence, teardown sequence (DEC-082/101). |
| `26c5ece` | The unit-decomposed teardown walk (DEC-100). **Resolves D-032.** |
| `e63bf5a` | The TSan stress-test diagnosis and its gate. |
| `790671f` | The placeholder entropy source (DEC-063) and the TSan default. |

### The exit gate, item by item

| §13 requirement | Where |
|---|---|
| Probe-retry histogram at realistic per-cluster occupancy | `PlacementTest.cpp` — reports fill by decile over a full 32 MiB cluster |
| Memory returns to baseline after a pumped churn cycle | `DeferredReleaseTest.cpp`, `OracleTest.cpp`, `AddressSpaceTest.cpp` |
| Coverage revalidation, incl. the full-unmap recycled-node variant | `RevalidationTest.cpp` |
| Teardown residue ≤ the DEC-096 bound, creation-failure paths exercised | `AddressSpaceTest.cpp` — the bound is ZERO until Phase 4's cache exists, and `assertNoLiveObjects` checks exactly that; the failure sweep covers every allocation index |
| Bucket-codec round-trip + `static_assert` gate | `BucketCodecTest.cpp` |
| The lost-CAS row | `GrowthTest.cpp` — four CPUs racing creation and growth |
| Record-population conservation at teardown | asserted inside `DeferredReleasePools::destroy` |
| **Carried forward from Phase 2**: the three disabled reader-side tests, and `rcuTortureForcedStall` | `ConcurrentTest.cpp` — all three on, mutation-verified |

### What Phase 4 needs to know

- **The teardown walk marks** (DEC-100). That was the point of `26c5ece`, and
  it is a hard prerequisite: landing the descent cache over a non-marking
  teardown is the exact defect §7.4 spends a paragraph on. `resumeDescent`'s
  `Detached` answer depends on it.
- **D-029 is a WATCH item.** DEC-069's release-plus-acquire-*fence* refcount is
  invisible to TSan, which does not model `atomic_thread_fence`; it is folded
  into an `ACQ_REL` RMW at both release sites. If a `Mapping`-constructor-vs-
  `offsetFor` race ever reappears, **do not re-silence it** — the alternative
  explanation is a real use-after-free.
- **The entropy source is a placeholder.** `kernel/include/Random.h` is not a
  CSPRNG and says so; it carries the checklist for replacing it. Placement takes
  entropy as a parameter, so the swap is two files.
- **D-030 is an accepted, recorded residue**: growth over an empty cluster
  strands the old root. Two nodes per cluster, recoverable by the ordinary path,
  and a gap between two individually-correct rules rather than a bug.
- **`CROCOS_SKIP_TSAN_STRESS` defaults ON.** The sixteen "flaky" Core TSan
  failures were machine-load starvation of wall-clock timeouts, diagnosed in
  `e63bf5a` — idle 441/441, loaded 425/441, reproducible both ways. Run them
  deliberately with `-DCROCOS_SKIP_TSAN_STRESS=OFF` on a quiet machine.

### Spec follow-ups owed

D-004 (§7.2's edge-subdivision row states a minimal-shape figure as general),
D-029 (§6.6 should record the fence form as correct-but-untestable and the
ACQ_REL form as the implementation's), D-030 (DEC-096's residue bound should
carry the stranded-root term, and §6.4's walk-termination paragraph should note
that the exemption is positional).

---

### A.1 Where things stood at Phase 3's close

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

### A.2 Prerequisites Phase 3 landed

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

### A.3 Obligations carried into Phase 3

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

### A.4 What Phase 2 learned — still the best list of lessons in this file

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

### A.5 Known flakes

- **Core/RCU TSan after a full parallel rebuild**: `run_all_tests` reports ~16
  timeouts (AtomicBitPool, TreiberStack, `rcuConcurrent*`). All pass when re-run
  serially. Documented; see `[[project_core_tsan_post_rebuild_flake]]`.
- **`rcuTortureDeadSlotDoesNotUnboundLimbo`** fails `residue <= kResidueBound`
  about **3 runs in 12** under TSan. Measured at the same rate with and without
  the radix work, so it is pre-existing and independent — it counts retired
  *objects*, not memory. Distinct from the timeout flake above and **not yet
  investigated**; a small standalone task if you want one.

---

### A.6 Conventions

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

### A.7 Phase 3's build order (historical)

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

### A.8 Commit history through Phase 2

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
