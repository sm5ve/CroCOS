# Bootstrap Memory Mapping Sequence

This document describes how the kernel establishes its virtual memory layout during the `memory_management` init phase, before the full page allocator and VMSubstrate are available.

## Problem

The page allocator needs a contiguous virtual buffer to store its per-domain metadata, but those buffers are physically scattered across NUMA nodes and have no virtual mapping yet. Setting up the mappings requires writing page table entries, but those tables themselves need physical pages to live in — a chicken-and-egg situation.

The solution: carve space for the page tables and buffer out of the top of the target physical range, map that space temporarily using `TempWindow`, initialize the tables, then install the new zone entry and tear down the temp window.

## TempWindow

`TempWindow<T>` provides temporary virtual access to an array of physical pages during bootstrap. It requires `sizeof(T) == arch::smallPageSize` — the type `T` names what each slot is interpreted as (typically a page table type).

### Recursive page table trick

The kernel places a single page table (`detail::tempWindowTable`) at `pageTableLevelForKMemRegion()`. Rather than allocating a second hardware-level table to hold the leaf entries, slot 0 of `tempWindowTable` is a self-referential subtable entry pointing back at `tempWindowTable` itself.

Because the present bit, leaf/subtable discriminator, and physical address encoding are consistent across all levels (enforced by `arch::recursivePageTablesSupported`), the CPU's page-table walk interprets the self-referential entry as a next-level table, making `tempWindowTable` visible as an array of leaf-level entries at virtual address `zoneBase`. Writing `zoneBase[i+1]` as a leaf entry is identical to writing `tempWindowTable[i+1]` — no second hardware table is needed.

```
zoneBase + 0*smallPageSize  →  tempWindowTable itself  (self-ref, slot 0)
zoneBase + 1*smallPageSize  →  physBase + 0*smallPageSize  (temp[0])
zoneBase + 2*smallPageSize  →  physBase + 1*smallPageSize  (temp[1])
...
```

`virtualBase()` returns `zoneBase + smallPageSize`, pointing at slot 0 of the mapped range.

### Lifecycle

**Constructor** (`TempWindow(phys_addr base)`):
1. Writes the self-referential subtable entry into `tempWindowTable[0]`.
2. Installs `tempWindowTable` as the zone entry for `TEMPORARY_AND_PAGE_TABLE_ZONE`.
3. No TLB flush is needed — both entries are new (CPUs do not cache not-present entries).

**`operator[](i)`**:
- Computes the leaf entry for `physBase + i*smallPageSize`.
- Writes it through the recursive mapping at `zoneBase[i+1]`.
- Calls `invlpg` on the target virtual address only if the entry changed.

**Destructor**:
1. Zeros all entries in `tempWindowTable` directly (the table lives in the kernel image, always accessible).
2. Clears the zone entry.
3. Issues a full TLB flush to retire all stale mappings.

The scoped block around `TempWindow` in `reservePageAllocatorBufferForRange` means the destructor runs before the newly-installed zone entry is used:

```cpp
{
    TempWindow<arch::PageTable<arch::pageTableDescriptor.LEVEL_COUNT - 1>> tmp(ptPhysBase);
    // initialize page tables through tmp ...
    getPageTableEntryForZone(zone) = newZoneEntry;
} // TempWindow destructor clears temp zone and flushes TLB, activating the new zone entry
// new zone is now live
```

## Page allocator buffer mapping

`reservePageAllocatorBufferForRange(range, size)` sets up one page allocator zone per call:

1. **Carve out** space at the top of the physical range:
   - `requiredTableSizeForPageAllocator` bytes for page tables.
   - `alignedBufferSize` bytes for the allocator buffer itself.

2. **Open a TempWindow** over the carved-out table space.

3. **Zero the table pages** through the TempWindow.

4. **Call `initializePageTable`** (from `BootstrapMapper.h`) to recursively populate the tables to map the buffer range. This handles aligned huge-page middle sections and recursion into subtables for unaligned head/tail portions.

5. **Install the zone entry** for the next `PAGE_ALLOCATOR_ZONE_START + n` slot.

6. **Close the TempWindow** (destructor flushes TLB), making the new zone live.

The returned virtual pointer into the new zone is passed to a `BootstrapAllocator` which hands out fixed-size chunks for constructing `NUMAPool` and `LocalPool` objects.

## `BootstrapMapper.h`

All bootstrap page table helpers live in `kernel/include/mem/BootstrapMapper.h` as header-only templates, so VMSubstrate can reuse them when it needs to map its own internal structures.

Key exports:

| Symbol | Purpose |
|--------|---------|
| `supportsSimpleBootstrapPageAllocatorMapping` | Compile-time check: all levels from `pageTableLevelForKMemRegion()` down can be leaves and all tables are multiples of `smallPageSize`. |
| `requiredTableSizeForPageAllocator` | Total bytes needed for page tables to map one zone. |
| `initializePageTable<level, upper>(base, range, ptPhys)` | Recursively populate a page table stack to map `range`. `upper=true` fills from bottom; `upper=false` fills from top, tracking the address offset. |

## End of bootstrap

After `initPageAllocator` returns, the init framework sets `earlyBootMappingExpired = true`. Any call to `early_boot_phys_to_virt` or `early_boot_virt_to_phys` after this point will assert.
