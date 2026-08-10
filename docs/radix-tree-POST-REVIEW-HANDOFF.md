---
kind: handoff
status: ready — all decisions closed (§7.1 approved by Spencer 2026-08-10)
audience: the next session
supersedes: docs/radix-tree-REVIEW-HANDOFF.md (deleted — it declared itself obsolete)
companion: docs/radix-tree-HANDOFF-impl.md (implementation reference, still current)
---

# radix-tree — where the branch stands

**Branch `radix-tree`, 80 commits, not merged, not pushed. `master` does not even
configure** (its `libraries/LibExt/CMakeLists.txt` names sources that do not exist
there), so this branch is required to restore a working default target.

Everything below §1 is the historical record of the pre-merge referee pass. **Read
§0 and stop; go to `docs/radix-tree-implementation-deviations.md` D-070 for detail.**

---

## 0. RESOLVED 2026-08-10 — the amendments stand

**`specs/radix-tree.md` §7.1's per-CPU population rule.** It normatively required the
population to be *"at least the per-operation ceiling"* (≈230). The shipping code uses
32, because D-059 made the population **also the per-attempt record budget** — an
attempt that would want more returns `NeedsDecomposition` and §6.5 splits it.

That contradiction was raised to Spencer with the four amendments marked
`PENDING SPENCER REVIEW`. **Spencer approved the amendments on 2026-08-10**: the
population is normatively 32, the ≈230 edge sum survives only as the derivation that
makes the budget necessary and as the draw histogram's top bucket, and the code stands
as shipped. The rejected alternative was reverting to a ceiling-sized population at
115 KiB per address space against 16 KiB, and 1,840 `tryMake` calls per fork against
256. All four markers are cleared; the spec text now reads
`APPROVED BY SPENCER 2026-08-10`.

**No decision is outstanding. The branch is merge-ready.**

Two smaller judgement calls that came with it:

- **`kDefaultRecordsPerCpu = 32` is one short for one shape.** A full-span clear of a
  32-valence *cluster root* takes 32 per-slot rows (§6.5 keeps a root's per-slot
  results rather than replacing it), so the "never decomposes" property holds only
  because the budget happens to equal 32 exactly. Fine today; it constrains any
  future tuning downward.
- **R-20 + R-21b, the stress-fixture widening**, is deliberately still open. The
  in-kernel stress caps a wide unmap at 16 granules, so `kDetachBudget = 64` has
  **zero in-kernel decomposition coverage** and its `max=17` is fixture arithmetic
  (now labelled as such, derived from `kWideGranules` so it cannot go stale). Doing it
  re-baselines the branch's own integration instrument, which is why I did not do it
  in the same breath as thirteen deviations.

## 0a. State, verified

| | |
|---|---|
| Tests | **1,666 green across eleven runners**, ASan and TSan |
| Kernel | builds Debug and Release (LTO); all four boot configs exit 0, zero panics |
| Stress | radix clean at `OPS=24` Release, max 2 draws/attempt over 4.77M; RCU clean at 9.6M retires/CPU |
| Deviations | D-001..**D-070** |
| RCU | `specs/rcu-phase-1.md` **P1-DEC-019** added (a real `barrier` defect, four phases old) |

Reproduce: `cd tests && cmake --build build --target run_all_tests -j8`. The
race-detector stress is opt-in by design — `cmake --build build --target
run_tsan_stress` (quiet machine only; 441 vs the default gate's 425).

## 0b. What this session did, in one paragraph each

**D-058/D-059 — option C.** ITEM-084's shared per-address-space reserve made CPU A's
progress depend on CPU B's records coming home, which the RCU engine has no primitive
for; that one coupling produced four liveness defects. Replaced by the record budget:
the pool's population *is* the per-attempt limit, so termination is local to one CPU
and `consecutiveShortfalls <= 1` is exact again.

**D-060 — R-2.** `tryAllocateSmallPage` forwarded default flags, and default flags
*panic*; the `return false` was dead code, so the entire "failable" family was
non-failable and address-space creation could panic the kernel from userspace.

**D-061 / P1-DEC-019 — an RCU engine defect.** `drainClaimedBag` popped a node before
running its deleter, so `barrier` could return with a deleter still in flight.
Unreachable without stealing, which is why the torture suite missed it for four
phases; only observable because D-059 restored an exact invariant.

**D-062..D-069 — the referee findings.** R-17 (LibExt broke `all`), R-16 (the
static-buffer ceiling, wrong three times, now derived and asserted per row), R-5 (the
lost-CAS test never called the oracle it claimed to), R-4 (the ordering scanner's
hand-maintained header list omitted `Claim.h`), R-6 (the deleters' freshness — the
filed mutation was green *for a correct reason*), R-9 (a RELAXED load consuming
unclaimed child pointers), R-13 (a freshness call on pinned memory corrupts), R-18,
R-19, R-21's cheap half.

**D-070 — the five-referee pass.** One merge blocker (§0), two ordering defects I
introduced, R-2's class surviving three lines above its own fix, and a long tail.

## 0c. Traps, and the ones I hit

- **Give every mutating referee its own `git worktree`.** Four shared one tree and
  interfered; one built against another's injected `throw` that silently disabled
  R-5's whole fix and reported a false leak.
- **One probe assert per build.** Two in one build and the earlier caller aborts
  first, making the later site read as dead code.
- **A page-granular freshness assertion is only as strong as its guarantee that
  nothing else on that page was touched.** Four rewrites, four different maskings.
- **A coverage floor on a total cannot detect a missing subset** — that is why the
  ordering scanner's own self-check never noticed two unscanned headers.
- **More rounds do not fix an unentered race.** A barrier does.
- Everything in §6 of the old text still applies: `CROCOS_RADIX_STRESS_OPS=24`,
  Release/LTO being more informative than debug, `-icount` forcing single-thread TCG,
  `quiesce(h)` before any count check.

## 0d. Open, ranked

1. ~~**§7.1's population rule**~~ — **CLOSED 2026-08-10.** Spencer approved the
   amendments; the population is normatively 32 (§0).
2. ~~**R-20 + R-21b**~~ — **CLOSED 2026-08-10 (D-071).** `kWideGranules` 16 -> 64 puts
   the widest attempt at 68 against a budget of 64, so §6.5's decomposition path now
   runs on the target for the first time (149 decompositions at default OPS, 1,441 at
   OPS=24) and `max` is bounded by the budget rather than by the fixture. The row
   space went 16 -> 32, restoring MAP_FIXED and lookup to their pre-D-055 rates.
   **The widening initially cost a 256x lifecycle regression** — page-granular
   densification made a wide region 1,024 applies, which cut OPS=24 from 1,025 cycles
   to 4 — fixed by densifying with 4 x 16 KiB records instead, which holds the wide
   region at the old fixture's cost. Caught only by A/B-ing at OPS=24; the default
   setting showed 4 cycles before and after.
3. ~~**`CLAUDE.md`**~~ — **CLOSED 2026-08-10.** Rewritten for the merged subsystem:
   RadixVM, RCU, vmsmalloc, the tests/ project and its eleven runners, the two opt-in
   runners, the exit-code contract, the `klog` interrupt-safety trap, the `-icount`
   caveats, and the `specs/` untracking. It also **corrected an actively wrong
   instruction**: it told the reader to `brew install x86_64-elf-gcc`, which silently
   discards every global constructor. Note `CLAUDE.md` is gitignored, so that fix is
   local-only and does not travel with the repo.
4. ~~**`RCU-proposal.md`**~~ — **CLOSED 2026-08-10.** Moved into the spec tree rather
   than into `docs/` as originally planned: it is a superseded *proposal* ("not yet a
   spec", and `rcu.md` supersedes it) and every citation of it comes from inside
   `specs/`. The `docs/` plan predated the spec tree becoming its own private repo,
   after which `docs/` is specifically the public record of decisions and not a home
   for drafting. Citations that said "project root" now say "this directory".
5. **R-12** — **design half CLOSED 2026-08-10 (D-073); implementation is
   scheduler-dependent.** `kernel/include/sched/Preemption.h` now carries the seam:
   two predicates and two RAII guards, all no-ops, with a `PreemptionGuard` applied
   over `DeferredReleasePool::pop` — chosen over a tagged head because it keeps the
   pool's single-consumer argument literally true rather than replacing it.
   vmsmalloc's and RCU's duplicate predicate pairs now delegate there, so a scheduler
   fills in one file instead of three. Mutation-proved reachable. What is left is a
   preempt-count in `CpuLocal`, a per-thread migrate-disable count, and four function
   bodies.
6. ~~**`SafePtr`'s two move semantics**~~ — **CLOSED 2026-08-10 (D-072).** Resolved
   toward copying in both: `SafePtr` is a non-owning view with no destructor and
   unrestricted copying, and freshness attaches to an access rather than to a pointer,
   so a move has nothing to transfer. All five special members are now explicit in
   both specializations. Pinned by
   `radix_safeptr_move_is_a_copy_in_both_specializations`, mutation-tested in **both**
   directions — including the half whose behaviour did not change, so a later
   "consistency" pass cannot unify them onto the nulling semantics unopposed.
7. One unidentified transient in ~16 suite runs at peak referee load (10/10 clean
   after). Most likely the known load-sensitive conflict-rate assertion in
   `radix_concurrent_disjoint_writers_do_not_interfere`. Capture the name if it
   recurs.

---

# Historical: the pre-merge referee pass

Everything below is the record of the pass that produced D-062..D-069, kept for its
findings table. Status columns are accurate; the surrounding prose predates D-070.

## Suite state at the time


**176/176, ASan and TSan, and the whole `run_all_tests` gate green (1,664 tests
across ten runners).** The in-kernel radix stress boots clean at
`CROCOS_RADIX_STRESS_OPS=24`, Release, exit 0.

This section previously said the suite was deliberately RED at 170-171/172 —
`radix_concurrent_readers_never_observe_a_torn_state` and
`radix_concurrent_disjoint_writers_do_not_interfere` failing on `DeferredRelease`
live-object accounting — and that the only acceptable fix was to remove the cause.
The cause was `promote()` allocating records that lived until teardown. Option C
deleted `promote()`, so the red went green the way D-058 required, and no
assertion was touched to get there.

---

## 1. Working tree — all committed

Everything the referee pass produced is now on the branch: the R-1 stop-gap, the
`~Harness` / `~StarvedPools` teardown fix, D-058, and option C with D-059.

---

## 2. Findings, ranked. 21 from the audit, plus two found while fixing

(the second being D-061's RCU `barrier` defect, which is not in the tables below —
it is not a radix finding)

Severity, then status. **Open** means nobody has touched it.

### Merge-blocking

| ID | Finding | Anchor | Status |
|---|---|---|---|
| **R-1** | Replenish gate skipped the RCU pump — unbounded silent spin in `munmap` in release | `CoreTree.h:2204` | **FIXED** — stop-gap, then closed properly by C |
| **R-2** | `tryReservePerDomainStaticBuffer` is not failable — userspace-triggerable kernel **panic** | `VMSubstrate.cpp:1180` → `:693` | **FIXED** (D-060) — and it was BIGGER than described: `tryAllocateSmallPage` itself panicked, so the whole failable family was non-failable and every unwind around it was dead code |
| **R-3** | Termination rests on `promote()` — allocation on the `munmap` path, contra DEC-068 | `DeferredRelease.h:585` | **FIXED** by C (D-059) |

**R-1.** `atFullPopulation()` asks whether the pool holds all the records it
owns; the question is whether the attempt can proceed. Identical under DEC-068
(pool == the 230 ceiling), meaningless at 32. The early return was **added** by
`45c13bb`, the same commit that invalidated the predicate — it survived review
because a correct older use of the same predicate sits three lines below.
Reproduced: gate present → 300 s timeout; gate removed → completes.

**R-2.** As filed: the data pages go through `tryAllocateSmallPage` with a clean
unwind, while the lazy subtable installer used the panicking `allocateSmallPage`,
and `assertNotReached` is `PANIC_NO_STACKTRACE` in **release too** — unlike
`assert`. So a fork storm at low memory panicked instead of returning ENOMEM.

**On fixing it, the defect turned out to be one layer deeper than either referee
saw.** `tryAllocateSmallPage` forwarded default flags, and default flags panic — so
the *whole* failable family was non-failable and its `return false` was dead code.
That is why two referees produced opposite-sounding descriptions and both were
right. `mm.h:59`'s comment was wrong in the direction that hides it: it claimed the
family hands back a null `phys_addr`, which describes a caller that gets control
back. See D-060; the root fix is one flag in two wrappers.

### Detector gaps — every one proven by mutation, not argued

| ID | Finding | Anchor | Proof |
|---|---|---|---|
| **R-4** | **FIXED** (D-065). `ProgressAudit.h` (35 atomics) was missing too. The header list is gone — the directory is enumerated, so the standing "add new headers to this file" chore is gone with it. Neither header needed a fix; both already used named constants |
| **R-5** | **FIXED** (D-064). Both discard sites covered (`:309` and `:229`). The single race detected the leak only 10/12 — a **barrier before each growth** took it to 20/20, and 20/20 pass clean. `nodeCount()`'s `>=` bound is exact now |
| **R-6** | **FIXED** (D-066) — and green for a CORRECT reason: those two `SafePtr`s wrap the retire subject, `onPreTouch` already refreshed it, and freshness is **page-granular**. The wrong "and NOTHING else" comments are corrected (closes R-19's `CoreTree.h:107` row). The load-bearing `Mapping`-side call is now asserted; making it non-vacuous took **four** measured attempts |
| **R-7** | D-057's own test is vacuous for the property it names | `DeferredReleaseTest.cpp:593` | `promo=3, perCpu=16` — promotion covers the demand, reserve never touched |
| **R-8** | §7.1's `barrier` remedy was unreachable in every fixture | `CoreTree.h:2211` | Refill ahead of the pump; partially addressed by the R-1 fix |

R-5's hole is closed: both files now assert the allocation oracle after an explicit
teardown. Two things learned doing it, in D-064 — the CAS was rarely being **lost**
at all (a barrier, not more rounds, is what made the detector reliable), and
`DecompositionTest`'s new assertions are hygiene rather than a closed finding, since
`discardUnusedAllocations` was already covered by the two concurrent-writer tests
and the only single-threaded decomposition discard is structurally unreachable.

### Latent — R-9 and R-13 now FIXED; only R-12 remains

| ID | Finding | Anchor |
|---|---|---|
| **R-9** | **FIXED** (D-067). Audited all five sites: the other three genuinely hold whole-node claims and now ASSERT it. The two failing ones take a new `kAttemptSlotLoad = ACQUIRE` |
| **R-12** | Treiber ABA safety holds only because **no scheduler exists** | `CoreTree.h:1412` |
| **R-13** | **FIXED** (D-068). Debug assert in `ensureTLBEntryFresh` + mirrored in the mock so it is TESTABLE, plus the four "carries no obligation" comments corrected to say *forbidden*. Nothing was doing it — this closed the door |

**R-9 is fixed** (D-067). It is worth keeping the reasoning: the constant claimed
"a writer reading a slot it already holds the claim bit for", and at two of five
sites the attempt held no bit. Unobservable in both directions — x86-64 gives every
load acquire semantics, and TSan on ARMv8 does not model a missing acquire on a
well-formed atomic — so it was findable only by reading. The precondition is now a
runnable `assert(claimsFullValence(...))` at the three sites that depend on it,
which is the part that stops it recurring.

**R-13 is fixed** (D-068). The finding was really about the documentation: every
statement of the pinned exemption said "carries no *obligation*", which invites the
conclusion that a call there is merely wasted. It is not — it read-modify-writes at
`tableBase + dw*4096 + k_abs*8`, and the static-buffer slot has no dirty bitmap by
design, so it lands in live pinned data. Now a debug assert, mirrored in the mock so
a unit test can drive the refusal — this bug class has eight members and **zero**
were ever found by tests.

### Removed by option C — CLOSED, the code they lived in is gone

| ID | Finding | Anchor |
|---|---|---|
| **R-8** | §7.1's `barrier` remedy was unreachable in every fixture | `CoreTree.h:2211` |
| **R-10** | `returnSurplus`'s `depth <= perCpu` early-out strands reserve loans | `DeferredRelease.h:561` |
| **R-11** | `promote()` partial failure re-fires forever and re-grows pools it already grew | `DeferredRelease.h:589` |
| **R-14** | `promote()` allocates under the framework-global spinlock (8,192 allocations at 256 CPUs) | `CoreTree.h:2202` |

R-19's `DeferredRelease.h:124` and `DeferredRelease.h:471` rows also close: the
single-consumer claim is true **by construction** again (`refillFromReserve` was
the second popper), and the "reserve is filled FIRST" comment described a reserve
that no longer exists.

### Hygiene, docs, evidence

| ID | Finding |
|---|---|
| **R-15** | `BlockPool`: no double-free detection; domain bound is debug-only over a release OOB write (`BlockPool.h:250`, `:278`) |
| **R-16** | **FIXED** (D-063). Measured **~180,795 at 8 CPUs, 5,939 B each**. The ~174,000 re-derivation was also wrong: the metric is each consumer's SHARE OF A PAGE, not its stride. The figure is now DERIVED by `radix_static_buffer_ceiling_is_derived_from_the_live_pools` and asserted per row — a bracket was tried and let a 9% regression through |
| **R-17** | **FIXED** (D-062). `Ext` is an INTERFACE library and both stubs are gone — they were stray `git add -A` additions in `87e4a2f`, and `master` had the mirror-image break (the CMakeLists named sources that did not exist). The reported error was actually host clang meeting cross-GCC's `-Wno-comma-subscript` under `-Werror`, not the `<iostream>` |
| **R-18** | `CROCOS_SKIP_TSAN_STRESS` defaults ON → 441 vs 425; EpochDomain loses race coverage in the default gate, re-creating by another route the exact condition the comment above it warns about |
| **R-19** | Six load-bearing comments state properties the code lacks — see below. **Four now fixed**: the two `DeferredRelease.h` rows by D-059, and the `CoreTree.h:107` page-granularity row by D-066 (in three places, not one). `CoreTree.h:3053`, `CoreTree.h:1540` and `mm.h:59` remain — the last of those is fixed by D-060 |
| **R-20** | D-055's workload change displaced coverage: MAP_FIXED −33%, lookup −50% (the only row exercising the descent cache and the `-icount` probes) |
| **R-21** | `detachBudget`'s measured "max 17" is the fixture's structural ceiling (`wide ? 16u` granules = one level-5 node = 17), not an observation. The analytic bracket in `Claim.h:60` carries the retention |

**R-19, the wrong comments:**

| Location | Claims | Actually |
|---|---|---|
| `DeferredRelease.h:471` | "The reserve is filled FIRST" | The loop runs `c = 0…cpus` with the reserve at `c == cpus` — **last** |
| `DeferredRelease.h:124` | Single-consumer "by construction" | By the domain-management lock for the reserve; by CPU ownership for per-CPU pools |
| `CoreTree.h:3053` | Commit performs §6.1's phase order | One per-slot loop interleaves all five; safety actually rests on the open read section |
| `CoreTree.h:107` | `onPreTouch` covers the head "and NOTHING else" | Freshness is page-granular, so it covers the whole node |
| `CoreTree.h:1540` | `tryAdvance` makes a bag drainable by any CPU | `tryAdvance` never seals; only the owner's next retire, its own `barrier`, or `drainAllQuiescent` |
| `mm.h:59` | `allocateSmallPage` returns null on exhaustion | It **panics** unless `GRACEFUL_OOM` |

---

## 3. Settled — do not re-litigate

**Both of the old handoff's self-suspicions resolved in the author's favour.**

- **Reserve pops are genuinely mutually excluded.** `refillFromReserve` is the
  only non-teardown popper and is always under `DomainManagementLockGuard`;
  `destroy()` is teardown-only. Single-consumer holds — **by the lock, not by
  construction**. Only the comment is wrong (R-19).
- **The `population` race is benign.** Monotone, so a stale read is only ever
  small; and TSO orders the `depth` store before it, so the dangerous direction
  is unobservable on x86-64. On ARMv8 the reverse yields a spurious extra
  barrier — the safe direction. Worth making `Atomic<size_t>` to close the UB.

Also verified sound and not to be re-checked: the freshness discipline itself
(all twelve `.address()`/`.raw()` extractions and seven raw `Mapping*` flows are
encode-, identity- or cast-only — **§1.3 of the old handoff can be closed**); the
claim order is total and growth-invariant; the `Mapping` refcount ledger traces
clean end to end; growth is genuinely retire-free; DEC-101's unwind is correct
with its three lock scopes properly separated; the packed codecs round-trip and
`isWellFormed` statically rejects overflow; the `EpochDomain` engine is
byte-identical apart from one `public:`; and there is **no build-flag
relaxation** anywhere on the branch.

---

## 4. The design decision — option C (DONE), then maybe D

> **Implemented. D-059 in the deviations log is the record.** What follows is the
> reasoning that chose it, kept because the root-cause paragraph below is the
> thing to re-read before anyone proposes a shared resource in this subsystem
> again. Two figures in the table were superseded by the real implementation:
> ITEM-084's record memory is 23 KiB rather than 30 KiB, and C's is 12 KiB rather
> than 16 KiB.

### The root cause, stated once

DEC-068's pool had a property nobody named because it was invisible until it was
gone: **the termination argument was LOCAL.** Each CPU held a full ceiling and
`barrier` drives the caller's own retirees, so "short → barrier → my records come
home → proceed" involved exactly one CPU.

ITEM-084 replaced that with one shared reserve, and **CPU A's progress can now
depend on CPU B's records coming home** — which the RCU engine has no primitive
for. `barrier` completes your own retirees. `tryAdvance` never seals a foreign
Open bag (owner-only by I13). `drainAllQuiescent` force-seals universally but
only at teardown with no readers.

That is why this produced four liveness bugs rather than one. **The coupling is
the defect; R-1, R-3, R-10 and the Open-bag stranding are symptoms.**

### Option C — a record budget drives §6.5 decomposition

Reuse the machinery that already exists for `detachBudget`: exceed the budget,
return `NeedsDecomposition`, split into units. Then no attempt can need more than
the per-CPU pool holds, the shared reserve is deleted, and DEC-068's local
termination argument returns at one-seventh the memory.

**`kDefaultRecordsPerCpu = 32` is already the right constant, for a mechanism
that was never built.** A single node's worst partial cover is 31 records at the
widest valence — exactly the reasoning that chose 32 — so decomposition to
single-node units always fits. The 230 edge sum exists only because one attempt
is currently allowed to span levels.

**Termination verified:** `decompose` splits at the site node into per-slot units
and strictly descends; a leaf-level unit needs at most one record. Record-driven
decomposition always converges.

### The corrected numbers

An earlier version of this comparison mixed per-CPU figures against totals and
made option D look better than it is. Per address space, 8 CPUs:

| | records | bytes/AS | `tryMake` per fork |
|---|---|---|---|
| DEC-068 | 8×230 | 115 KiB | 1,840 |
| ITEM-084 (today) | 8×32 + 230 | 30 KiB | 486 |
| **C** | 8×32 | **16 KiB** | 256 |
| D alone (B=16) | 8×15 | 37.5 KiB | 120 |

**D alone is worse than ITEM-084 on memory**, because batching wastes 14 of 16
entries on the common two-mapping operation. And C+D do not "multiply down" in
practice: they multiply the correctness *floor*, but the pool still needs
throughput headroom, and at 2 records per CPU you would barrier every other
`munmap`.

### Option D — deferred, not rejected

One record per *attempt* carrying an inline array of `(Mapping*, delta)` pairs.
Its real benefit is fewer retire subjects and less RCU bag pressure, not memory.
Needs a §7.1 amendment (currently "one record per DISTINCT `Mapping`") and
supersedes D-056/D-057. **Evaluate with measurements after C**, once you can see
whether a 32-record pool ever shortfalls.

---

## 5. Option C — implementation notes (all six done)

1. **Add a record budget** and raise `NeedsDecomposition` (not `NeedsRecords`)
   when `deferMappingRelease` would exceed it. The draw site is
   `CoreTree.h:2057`; the status plumbing already exists at `:2447`.
2. **Delete the reserve machinery**: `refillFromReserve`, `returnSurplus`,
   `SurplusGuard`, `promote`/`shouldPromote`, `kPromotionThreshold`,
   `kShortfallRoundLimit`, `shortfallEvents`, `noteShortfall`, and the `+1`
   reserve slot in `deferredReleasePoolBytes`.
3. **Restore the exact invariants ITEM-084 weakened**: `consecutiveShortfalls
   <= 1` becomes exact again (reversing D-057), and `destroy()`'s conservation
   check goes back to **per-pool** — the weakening was forced only by refills
   moving records between pools.
4. **`replenishRecords` collapses** to the pre-ITEM-084 shape, which the R-1 fix
   has already restored the back half of.
5. **The red goes green legitimately** when `promote()` is gone.
6. **Then** write the regression test — against the surviving design, where the
   assertion is "no unit ever exceeds the budget", which is far easier to make
   non-vacuous than "the pump rescued a drained reserve".

---

## 6. Traps that cost time this session

- **`setPageAllocFailAt` does NOT close promotion's escape hatch.** It fails the
  page allocator beneath vmsmalloc; a 64 B record comes from an existing partial
  slab, and a wide clear has just freed many of the same size class. Use
  `oracle::shouldInjectFailure`, which fails `tryMake` itself. A first regression
  test asserted `promotionCount() == 0` and got `promo=3, perCpu=16` — the fifth
  instance of this subsystem's recurring lesson, in the reviewer's own work.
- **Teardown asserts used to mask test failures.** Fixed this session — both
  `~Harness` and `~StarvedPools` threw from implicitly-`noexcept` destructors, so
  a throw during an unwind was `terminate()` (original failure lost, everything
  blamed conservation) and an escaping throw skipped `domain.deinit()`,
  `resetDomainManagementState()` and `shutdown()`, corrupting the next test.
- **`assert` is compiled out in release** (`kassert.h:51`), but
  `assertNotReached` is **not** — it is `PANIC_NO_STACKTRACE` in both builds.
- **There is no timer-driven RCU pump.** Every `tryAdvance` call site is on an
  operation path, so a spinning CPU advances nothing.
- Everything in the old handoff's §4 still applies: `CROCOS_RADIX_STRESS_OPS=24`,
  Release/LTO being more informative than debug, `-icount` forcing single-thread
  TCG (74% vs 7% cache hit — read every locality figure there as a best case),
  a single page not making a deep tree, `quiesce(h)` before any count check, and
  new radix headers needing to be added to `OrderingSpellingTest.cpp` — which,
  per R-4, **`Claim.h` never was**.

---

## 7. Where the evidence lives

- **D-058** in `docs/radix-tree-implementation-deviations.md` — the long-form
  account of R-1, the harness defect, the red suite and the `setPageAllocFailAt`
  trap.
- **The review report** —
  https://claude.ai/code/artifact/33744d59-e212-44c7-a4f8-3df155145404 — the
  consolidated referee findings with the mutation table.
- `docs/radix-tree-HANDOFF-impl.md` — implementation reference, still current.
- `specs/radix-tree.md` §7.1 — freshly rewritten and Spencer-approved; **option C
  does not disturb it**, which is one of C's advantages over D.
