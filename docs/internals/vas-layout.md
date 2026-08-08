# Kernel Virtual Address Space Layout

CroCOS divides the 64-bit virtual address space between user processes (low half) and the kernel (high half). This document covers only the kernel portion.

On AMD64 with 4-level paging the usable virtual address space is 48 bits, with canonical form requiring bits [63:48] to sign-extend bit 47. The kernel occupies the top two root-table slots.

## High-level map (AMD64)

```
[0xFFFFFF8000000000, 0xFFFFFFFFFFFFFFFF]  512 GiB   root[511]  kernel zones
[0xFFFFFF0000000000, 0xFFFFFF7FFFFFFFFF]  512 GiB   root[510]  VMSubstrate
[                 ... user space ...                           ]
```

`VMM_SUBSTRATE_ROOT_INDEX = entryCount[0] - 2` keeps the slot assignment derived from the descriptor rather than hardcoded.

## Kernel zones (root[511])

The 512 GiB under root[511] is subdivided into 1 GiB zones, one per entry in the next-level table. Zones are numbered from the top of the address space downward; zone _n_ starts at `getKernelMemRegionStart(n)`.

```
zone 0  KERNEL_ZONE                [0xFFFFFFFFC0000000, 0xFFFFFFFFFFFFFFFF]  kernel image
zone 1  TEMPORARY_AND_PAGE_TABLE_ZONE  [0xFFFFFFFF80000000, 0xFFFFFFFFBFFFFFFF]  bootstrap temp window
zone 2  PAGE_ALLOCATOR_ZONE_START  [0xFFFFFFFF40000000, 0xFFFFFFFF7FFFFFFF]  page allocator domain 0
zone 3                             [0xFFFFFFFF00000000, 0xFFFFFFFF3FFFFFFF]  page allocator domain 1
...
```

Zone 1 is a bootstrap-only region used by `TempWindow` and is inactive after the `memory_management` init phase completes. Zones 2+ each hold the metadata buffer for one NUMA page allocator domain; a new zone is installed for each domain as its buffer is mapped.

### Zone address arithmetic

```cpp
constexpr size_t getKernelMemRegionSize()        // 1 << 30 = 1 GiB on AMD64
constexpr virt_addr getKernelMemRegionStart(n)   // -(n+1) * regionSize, canonicalized
constexpr virt_memory_range getZoneVirtualRange(n)
```

`pageTableLevelForKMemRegion()` returns the descriptor level whose coverage just exceeds `MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2` (28 bits = 256 MiB). On AMD64 this is level 2 (PD), giving 1 GiB regions.

### Early-boot identity mapping

During boot, physical address _p_ is also accessible at `kStart + p` where `kStart = getKernelMemRegionStart(0)`. The helper functions `early_boot_phys_to_virt` and `early_boot_virt_to_phys` use this fixed offset. Both assert that `earlyBootMappingExpired` is false; the init framework sets this flag at the end of the `memory_management` phase to catch any callers that outlive the early mapping.

## VMSubstrate region (root[510])

The 512 GiB VMSubstrate region holds internal virtual memory manager structures: page tables, region descriptors, and slab allocator state. Its root subtable (`vmmAreaTable`) is a statically allocated next-level table installed at root[510] during `VMSubstrate::init()`.

The region is intentionally separate from the kernel zones so that VMSubstrate's own page table management cannot accidentally alias kernel image or page allocator memory.
