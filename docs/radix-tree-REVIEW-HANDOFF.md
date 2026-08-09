---
kind: handoff
status: ready
audience: a team of referee subagents
---

# radix-tree — review handoff, before merge

**Written 2026-08-09 at `5d74670`.** Branch `radix-tree`, **61 commits ahead of
`master`, never merged**. Phases 0–5 all complete; every spec item closed or
consciously retained; suite 172×2 green on both sanitizers; six clean kernel
boots (`run`, `run_numa`, `run_numa_hmat` × debug and Release/LTO).

The next session's job is **not implementation**. It is a referee pass over this
branch before it merges. This document exists to make that pass efficient: it
says where the risk is concentrated, what has already been settled and must not
be re-litigated, and — most usefully — **where I already suspect my own work**.

A referee team should not spread itself evenly over 61 commits. The risk is very
unevenly distributed and §1 ranks it.

---

## 0. The one lens worth applying everywhere

**A test that cannot produce a shape reports success, and success looks like
evidence.** This subsystem produced that failure four separate times in one
session:

1. Six freshness sites were invisible to a harness whose `ensureTLBEntryFresh`
   was a no-op that recorded nothing — 150 tests passed with every call absent.
2. The `DeferredRelease` draw histogram read "max 2" from a stress that only ever
   unmapped exactly what it placed — a *full* cover, which §7.1 says is the cheap
   row. The expensive shape is a *partial* cover, which that workload cannot make.
3. The detachment histogram read "zero" **twice**: once because no row fully
   covered a child, and again because granule-sized records are stored as leaves
   (`kPlacementGranularity` is exactly `nodeSpan(level 6)`) and subdivide nothing.
4. `consecutiveShortfalls <= 1` was falsified by concurrency that every existing
   reserve test was single-CPU and structurally could not create.

So the question to ask of any test, assertion or measurement on this branch is
not "does it pass?" but **"what shape can it not produce, and does that shape
matter?"** Every calibration figure in D-055 is deliberately recorded together
with the workload that produced it, for exactly this reason.

---

## 1. Where the risk is, in order

### 1.1 HIGHEST — the ITEM-084 pool redesign (D-056, D-057)

Commits `45c13bb`, `6953baf`, `5d74670`. Newest code on the branch, least soak
time, and it changed a **shipped, previously-green allocation path**. I found
three defects in my own design while building it, which is itself the argument
for reviewing it hardest: a design space that yielded three traps to its author
probably has more.

What changed: per-CPU `DeferredRelease` pools dropped from the per-operation
ceiling (230) to `kDefaultRecordsPerCpu = 32`, behind **one shared reserve per
address space** holding the full ceiling. The reserve carries §7.1's replenish
termination argument; the per-CPU size only decides how often it is touched.

**Two things I already suspect and could not fully settle. Start here.**

- **The Treiber stack's ABA-safety argument no longer holds "by
  construction".** `DeferredReleasePool`'s own comment says: *"Multi-producer
  (any CPU's deleter pushes), SINGLE-CONSUMER (only the owning CPU draws). That
  asymmetry is what makes a plain Treiber stack ABA-safe here … reaching a
  previously popped node again requires a second popper. There is none, by
  construction."* **The reserve now has a second popper**: any CPU refilling from
  it. I believe it is still safe, because every reserve pop goes through
  `refillFromReserve`, which is only ever called under the domain-management lock
  (`CoreTree.h:2187–2194`), so pops are mutually excluded and single-consumer is
  preserved *by the lock rather than by construction*. **Verify that claim
  independently**, and check whether any path can pop the reserve without the
  lock. If it holds, the comment needs amending; if it does not, this is an ABA
  bug in a lock-free stack, which is the worst-shaped defect on the branch.

- **`atFullPopulation()` reads `population` outside the lock while `promote()`
  writes it.** `population` is a plain `size_t`. `promote()` increments it under
  the domain-management lock (`CoreTree.h:2202`), but `atFullPopulation()` is
  called at `CoreTree.h:2204` and `2216`, both *outside* the lock scope. I
  believe it is benign — the value only grows, and a torn `size_t` read is not a
  thing on the targets — but it is a data race by the letter, TSan has never seen
  it (no test promotes concurrently), and "benign" is a judgement a referee
  should make rather than inherit.

Also worth adversarial attention, in rough order:

- **`returnSurplus` (`DeferredRelease.h`)** drains the CPU's pool completely and
  re-files each record by `homePool`. Check: the pop side is owner-only (fine
  against concurrent lock-free pushes from foreign deleters), natives return to
  the owner's pool and loans to the reserve, and the routing cannot lose or
  duplicate a record. It is O(depth) under the lock — bounded by 230+32, but it
  is work under a lock the whole framework shares.
- **The `SurplusGuard` destructor takes the domain-management lock.** Check it
  cannot run while the lock is already held, or inside a read section. It fires at
  the end of `runToCompletion`, which should be outside both.
- **Teardown's conservation check was weakened** from per-pool to a total
  (`destroy()`), because a refill legitimately moves records between pools. It
  still catches "a record was drawn and never returned"; confirm it cannot now
  miss a record that moved to the *wrong* pool and stayed there.
- **`destroy()` was made idempotent** because DEC-101's creation unwind can reach
  it twice. Check the unwind paths actually agree with that.
- **Liveness under concurrent wide draws** (D-057). My argument is: a refill asks
  for the *whole* reserve so two CPUs can never each hold half and stall, and
  RCU's domain-wide sweep (RCU-DEC-006 stealing) lets a waiter's own `drain` run
  the borrower's deleters. **Attack both halves.** In particular: is there any
  path where a borrower holds records without retiring them and without reaching
  `returnSurplus`?

### 1.2 MODERATE — `BlockPool` and the root-page move (D-051)

Commit `d7856cf`. Touched the RCU slot block and the radix control block
together, and moved the root bucket page into pinned storage.

- **`blocksAreImmutablyMapped` is load-bearing.** `ClusterTable::buckets` and
  `CoreTree::buckets` return **raw** pointers — no `SafePtr` — justified solely by
  that trait. Check every raw read of the bucket page really is on pinned
  storage, and that no path can hand a tree a bucket page from anywhere else.
- **Lock discipline at creation and teardown.** The root page's draw sits in its
  own lock scope *after* `Domain::init`, and its return in its own scope *before*
  `deinit`, because both take the lock themselves. Merging either is a
  self-deadlock. Verify the scopes.
- The window arithmetic (4,096 B/AS for its own pool vs 8,192 B folded) is
  recorded in D-051; re-derive if you doubt it.

### 1.3 MODERATE — the two `Mapping` signature changes (D-050)

Commit `aaca6bc`. `apply`'s incoming `Mapping*` and `enumerateChunk`'s outgoing
one became `SafePtr<Mapping>`. Mechanical and compiler-enforced, but check the
**encode sites** that call `.address()` — those deliberately discharge nothing,
and one that should have been a real access would be silent.

### 1.4 LOWER — measurement and instrumentation (D-052 … D-055)

Commits `c66839e`, `68e05f4`, `bbefb4d`, `51759ed`. Mostly additive and
flag-gated (`CROCOS_RADIX_DRAW_HISTOGRAM`, `CROCOS_RADIX_INSN_PROBE`), off in a
production kernel. Two things still deserve a look:

- **The stress's workload changed** — a bulk-unmap row and a dense-region row
  were added. Confirm the new mix did not displace coverage the old one had.
- **The freshness recorder** (`MockFreshness.cpp`) is thread-local and armed
  explicitly; disarmed it is one bool test. Confirm it cannot perturb the
  concurrent runs.

### 1.5 LOWEST — documentation and spec commits

`41831e3` (the §7.1 rewrite, Spencer-approved), plus the handoff and deviations
updates. Review for accuracy against the code, not for design.

---

## 2. Settled — do not re-litigate

Each of these was decided by Spencer or proven, and re-arguing them wastes the
pass. Challenge the *implementation* freely; the *decision* is closed.

| Decision | Basis |
|---|---|
| **D-042 option 1** — keep per-level node freshness | Spencer, on measurement: 1.00 calls/level, 24 instructions each, ~10–20% of the fault path (D-052/D-053) |
| **D-039/D-051** — root bucket page into its own pinned pool | Spencer-directed |
| **ITEM-084** — small per-CPU pools behind one reserve, with adaptive promotion | Spencer-directed |
| **§7.1's rewrite** — obligation attaches to the memory, not the caller | Spencer-approved |
| **ITEM-084 option 2 is dead** — decomposition does *not* bound an attempt below the edge sum | Proven and independently verified: the rows that draw records consume no detachment budget, and the row that does draws none (`CoreTree.h:2954-2961`). True per-attempt max 228 vs the derived 230 |
| **ITEM-055/002/031 consciously retained** | False-sharing questions; TCG models no cache, so `-icount` — which ITEM-055 itself names — is the wrong method |

---

## 3. Numbers I picked rather than derived

Every one of these is a defensible guess, not a measurement. A referee is welcome
to argue any of them; none is load-bearing for correctness.

- `kDefaultRecordsPerCpu = 32` — reasoned as one node's worst partial cover at
  the widest valence (a 32-slot node, 31 cleared). Never measured against 8 or 16.
- `kPromotionThreshold = 4` — picked.
- `kShortfallRoundLimit = 64` — picked, replacing an exact invariant with a
  generous defect detector (D-057).
- **Promotion never demotes.** Deliberate — dense allocation is a property of a
  process, not a phase — but unexamined for long-lived processes that change
  phase.

---

## 4. Practicalities

```
# unit tests — from tests/, NOT the repo root
cmake --build build --target run_all_tests      # 172x2 + the rest

# kernel, with the in-kernel stress
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DCROCOS_RADIX_STRESS=ON
cmake --build cmake-build-debug --target run    # or run_numa / run_numa_hmat

# instruction probes (D-053)
cmake -B cmake-build-o2 -DCMAKE_BUILD_TYPE=Release -DCROCOS_RADIX_STRESS=ON \
      -DCROCOS_RADIX_INSN_PROBE=1               # 1 call / 2 whole lookup / 3 count
cmake --build cmake-build-o2 --target run_icount
```

Traps that cost time this session, and would cost a referee the same:

- **`-DCROCOS_RADIX_STRESS_OPS=24`** turns a debug boot's 4 cycles into thousands
  with every assert live. The most useful debugging lever here.
- **Release/LTO is far more informative than debug** in the same 20 s window; both
  Phase 5 defects appeared only there. A hang shows as no `Goodbye :)` plus the
  full timeout — grep the verdict line, not the exit code.
- **`-icount` forces `-accel tcg,thread=single`**, and single-threaded TCG reports
  a **74% descent-cache hit rate where MTTCG reports 7%** — so any
  locality-sensitive figure read under `-icount` is a best case.
- **A single page does not make a deep tree** (D-038), and **one record does not
  make a subdivided one** (D-055): a record covering a slot's whole span is a
  leaf. Fixtures wanting depth need page-granular records, and fixtures wanting
  the expensive draw shape need *distinct* records in adjacent slots.
- **Any count-checking validation must `quiesce(h)` first** — releases are
  deferred.
- `CROCOS_SKIP_TSAN_STRESS` defaults ON.
- New radix headers must be added to `OrderingSpellingTest.cpp`'s list, or their
  atomics are invisible to §11's spelling check.

---

## 5. Reference

- `docs/radix-tree-implementation-deviations.md` — **D-001…D-057**, the findings
  log. D-054 through D-057 are this session's and carry the reasoning behind
  everything in §1.1.
- `docs/radix-tree-HANDOFF-impl.md` — the implementation handoff: phase state,
  what changed outside the tree (`SafePtr`, `klog`'s interrupt-context rule,
  `PinnedBlockPool`), and §A's history.
- `specs/radix-tree.md` — §7.1 is freshly rewritten; §14's items are all closed
  or consciously retained; the decision record carries the calibrated values.
- `specs/radix-tree-phase-5.md` — `status: complete`, with the close-out note.
