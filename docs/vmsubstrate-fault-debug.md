# VMSubstrate Fault Debug — Working Hypotheses

## Symptom

Page fault accessing `0xffffff0180201040` while running `allocPage()` on (presumably) CPU 6.
Reported call site: `reserveLeafBit` as called from `ensureSubtableInstalled`.

## Address decode

- `arenaBase` for CPU 6 = `0xffffff0180000000`
- Fault offset = `0x201040`
- Occupancy buffer base = `arenaBase + kSelfRefSize + D*4096` = `arenaBase + 0x201000` (with `D=1` on 8-CPU build)
- `0x201040 - 0x201000 = 0x40 = 64 bytes = leafAllocBitmap[T=8]`
- `T=8 = selfRefBitmaps`, the leaf bitmap covering PD[1]'s data range
- The page that backs this VA is slot 1 of CPU 6's chain leaf PT (phys `0x66400000` per TLB dump),
  which should map `arenaBase+0x201000 → occBufPhys[0]`

## TLB dump observation

Per-arena, only two entries are cached:
- `arenaBase+0` → arena root PD phys (self-ref, slot 0)
- `arenaBase+0x1000` → chain leaf PT phys (self-ref of arenaRoot[1])

No buffer-page entries (`+0x200000` dirty, `+0x201000+` occupancy) appear.
This is consistent with the page walker *failing* on those VAs (no positive entry caches), not with
them being cached and stale.

## Hypotheses (ordered by likelihood)

### H1 — Misattributed function: probably `claimLeafBit`, not `reserveLeafBit`

`ensureSubtableInstalled<2>` for `va` in PD[1] reads `arenaRoot[1]`. If `.isPresent()` is true
(chain leaf PT was installed in `createArena`), the function returns without calling `reserveLeafBit`.
After it returns, `reserveFreeVA` calls `claimLeafBit(arenaBase, 8)` (VMSubstrate.cpp:602), which
loads the same `leafAllocBitmap[8]` address. Both paths fault on the same byte; need to
double-check which frame is on top.

**Sub-case H1a**: If `parentEntry.isPresent()` *incorrectly* returns false on `arenaRoot[1]`,
`ensureSubtableInstalled` would:
1. Allocate a new phys page and overwrite `arenaRoot[1]` to point to it.
2. Install only dirty pages at slots `[0, D)` of the new leaf PT (no occupancy buffer pages!).
3. Call `reserveLeafBit(arenaBase, T_first=8, bit=0)`, which reads `leafAllocBitmap[8]`
   at `arenaBase+0x201040`. With slot 1 of the new leaf PT non-present, this faults.

This would actually match the user's report ("reserveLeafBit as called in ensureSubtableInstalled")
and would *cause* the slot-1-not-mapped state we see in the TLB.

### H2 — Chain leaf PT slot 1 is not what we think it is

Even without H1a, the entry at slot 1 of CPU 6's chain leaf PT might be malformed:
- `arch::PTE<leafLevel>::leafEntry(...)` not setting the present bit, or putting the phys field in
  the wrong position.
- `PageAllocator::allocateSmallPage(cpu)` returning duplicate phys pages — e.g., the chain leaf PT's
  own page colliding with `occBufPhys[0]` so the entries get clobbered.
- `TempWindow` writes not landing on the page we expect.

Easy probe: read `*(uint64_t*)0x66400008` and check format.

### H3 — TLB dump's absence of buffer entries confirms walk failure

The bootstrap CPU writes to the buffer in `seedAvailableState`, but `arch::flushTLB()` at the end of
`init()` purges those. A successful walk on CPU 6 *would* repopulate them. Their absence is consistent
with the walk genuinely failing — i.e., a leaf-PT entry really is non-present.

### H4 — `reserveBootBits` correctness (user's concern)

Walked the math: `selfRefBitmaps=8`, `D=1`, `kOccupancyBufferPages=18`. Loop reserves bits `0..18` of
`T=8`. Bit 0 → `arenaBase+0x200000` (dirty page). Bits 1..18 → `arenaBase+0x201000..0x212000`
(18 buffer pages). The static_assert at line 163 caps `D + kOccupancyBufferPages ≤ 64`. So the dirty
+ buffer span is correctly reserved within the single `T=8` bitmap. **Not a bug** as written, but
fragile if `kOccupancyBufferPages` ever pushes the sum past 64.

### H5 — Latent bug in `ensureSubtableInstalled` invlpg math (line 495)

`arch::invlpg(childTableAddr + dw * arch::smallPageSize)` flushes the *self-ref VA of the leaf PT
page itself plus dw*, walking across self-ref VAs of other (not-yet-installed) leaf PTs. If the
intent was to flush the dirty pages' data VAs, the math should be
`(childTableAddr - arenaBase) * kEntryCount + dw * smallPageSize + arenaBase`. Probably benign today,
but wrong.

### H6 — Per-CPU view sanity

Confirm fault is on the CPU that owns this arena. `reserveFreeVA` uses `getCurrentProcessorID()`;
a stale per-CPU ID would steer into someone else's arena.

---

## Root cause confirmed (2026-05-08)

`PageAllocator::allocateSmallPage(cpu)` is returning physical addresses that are not backed by RAM.

Evidence:
- QEMU is launched with `-m 256M`, so valid phys is `[0, 0x10000000)`.
- TLB dump shows chain leaf PTs at:
  - CPU 0: `0x64900000` (~1.6 GiB) — out of range
  - CPU 1: `0x0FE1A00000` (~63.5 GiB) — wildly out of range
  - CPU 2..7: similar 1.4–1.7 GiB range
- Direct probe: `xp /1gx 0x66400008` → `Cannot access memory`. The chain leaf PT page itself
  doesn't exist in RAM.

Meanwhile, the arena-root-PD allocations (also from `allocateSmallPage`) come back in plausible
range (`0x4a5000`–`0x538000`). So the bug is intermittent in `allocateSmallPage` — only some
allocations return junk addresses. Likely candidates:
- Per-CPU page pool corruption / wrap-around.
- Big-page-to-small-page split returning the wrong base.
- Free-list corruption such that the head pointer dereferences into an unmapped region.
- Truncation/extension bug where high address bits leak in (e.g., 32-bit write into a 64-bit
  field, or sign-extension of a 32-bit value).

The page fault and the entire H1a chain is the *symptom*; the underlying problem lives in
`kernel/mm/PageAllocator.cpp` (or wherever per-CPU small-page allocation is implemented).

### Recommended next steps

1. Instrument `allocateSmallPage` to log every returned address with the calling CPU and a
   running counter, then compare against the system's physical memory map.
2. Inspect the per-CPU page pool state right after `init()` returns — specifically the cursor /
   free-list head for CPUs 0..7.
3. Check whether the bogus addresses correlate with a particular pool drain / refill boundary
   (e.g., the 19th allocation of a CPU, since createArena allocates 18 buffer pages then a leaf
   PT).
