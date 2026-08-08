# Page Table Abstraction

CroCOS's page table layer is entirely architecture-agnostic. Architecture ports describe their hardware by filling in a `PageTableDescriptor` constant; all higher-level code (bootstrap mapper, TempWindow, VMSubstrate) operates on that descriptor through the generic API.

## Core types

### `PageTableDescriptor<N>`

Defined in `kernel/include/arch/PageTableSpecification.h`. Describes a page table hierarchy with `N` levels.

```cpp
template <size_t N>
struct PageTableDescriptor {
    static constexpr auto LEVEL_COUNT = N;
    PageTableLevelDescriptor levels[N];  // levels[0] is the root (top) table
    size_t entryCount[N];                // must be powers of two
};
```

`levels[0]` is the root table; `levels[N-1]` is the leaf-page table.

Key methods:

| Method | Returns |
|--------|---------|
| `getTableSize(level)` | Byte size of one table at `level` |
| `getVirtualAddressBitCount(level)` | Bits of virtual address space covered by one entry at `level-1` |
| `canonicalizeVirtualAddress(addr)` | Sign-extend address from the MSB of the virtual space |

### `PageTableLevelDescriptor`

Describes one level of the hierarchy. Notable fields:

- `entryWidth` — bits per entry (8, 16, 32, or 64)
- `present` — bit index of the present bit
- `canBeLeaf` / `canBeSubtable` — whether entries at this level can be leaves / subtable pointers
- `leafIndexBit` / `isLeafOnOne` — the bit and polarity that distinguish leaf from subtable entries
- `leafEncoding` / `subtableEncoding` — physical address encoding for each entry type

### `PageTableEntry<encoding>` (`arch::PTE<level>`)

A typed wrapper around a single page table entry. Constructed via:

```cpp
PTE<L>::leafEntry(phys_addr)      // leaf (page) entry
PTE<L>::subtableEntry(phys_addr)  // subtable pointer entry
```

Key methods: `markPresent()`, `enableWrite()`, `enableExecute()`, `markGlobal()`, `isLeafEntry()`, `getPhysicalAddress()`, `setPhysicalAddress()`.

The convenience aliases in `arch.h` derive from the architecture's concrete descriptor:

```cpp
template <size_t level>
using PTE = PageTableEntry<pageTableDescriptor.levels[level]>;

template <size_t level>
struct PageTable { PTE<level> data[pageTableDescriptor.entryCount[level]]; };
```

## Recursive page table invariants

`supportsRecursivePageTables(desc)` checks four conditions required for the self-referential page table trick:

1. **Uniform table size** — every level's table fits in exactly one leaf page (`getTableSize(i) == smallPageSize` for all `i`).
2. **Consistent present bit** — the present bit occupies the same position across all levels.
3. **Consistent leaf discriminator** — all levels that can be subtables share the same `leafIndexBit` and `isLeafOnOne` polarity.
4. **Matching address encoding** — `subtableEncoding.physAddrLowestBit`, `.physAddrTotalBits`, and `.addrStartInEntry` at every subtable-capable level match the bottom-level `leafEncoding`.

Invariant 4 is what makes the trick work: a self-referential subtable entry at level _k_ is also a valid leaf entry at the bottom level, so the CPU's page walker treats `tempWindowTable[0]` as a pointer to another table, which just happens to be `tempWindowTable` itself.

`arch::recursivePageTablesSupported` is a named `constexpr bool` computed from the architecture descriptor. `TempWindow` static-asserts on it at instantiation time.

## Architecture port

An architecture port declares its descriptor as a `constexpr` in its own header and assigns it to `arch::pageTableDescriptor` in `arch.h`. No other file needs to know the concrete type.

AMD64 (4-level paging):

| Level | Hardware name | Entries | Coverage per entry |
|-------|--------------|---------|-------------------|
| 0 | PML4 | 512 | 512 GiB |
| 1 | PDPT | 512 | 1 GiB |
| 2 | PD  | 512 | 2 MiB |
| 3 | PT  | 512 | 4 KiB |

Small page: 4 KiB. Big page (level-2 leaf): 2 MiB. `recursivePageTablesSupported = true`.
