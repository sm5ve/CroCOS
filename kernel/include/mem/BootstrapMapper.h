//
// Created by Spencer Martin on 4/27/26.
//

#ifndef CROCOS_BOOTSTRAPMAPPER_H
#define CROCOS_BOOTSTRAPMAPPER_H

#include <arch.h>
#include <kmemlayout.h>
#include <assert.h>

namespace kernel::mm {

    static_assert(pageTableLevelForKMemRegion() < arch::pageTableDescriptor.LEVEL_COUNT);

    template <size_t level>
    constexpr bool allHigherLevelsCanBeLeaves = [] {
        if constexpr (level >= arch::pageTableDescriptor.LEVEL_COUNT) return true;
        else return allHigherLevelsCanBeLeaves<level + 1> && arch::PTE<level>::canBeLeaf();
    }();

    template <size_t level>
    constexpr bool allHigherLevelTablesAreSmallPageSize = [] {
        if constexpr (level >= arch::pageTableDescriptor.LEVEL_COUNT) return true;
        else return allHigherLevelTablesAreSmallPageSize<level + 1> && (sizeof(arch::PageTable<level>) == arch::smallPageSize);
    }();

    template <size_t level>
    constexpr bool allHigherLevelTablesAreSmallPageMultiples = [] {
        if constexpr (level >= arch::pageTableDescriptor.LEVEL_COUNT) return true;
        else return allHigherLevelTablesAreSmallPageSize<level + 1> && (sizeof(arch::PageTable<level>) % arch::smallPageSize == 0);
    }();

    constexpr bool supportsSimpleBootstrapPageAllocatorMapping =
        allHigherLevelsCanBeLeaves<pageTableLevelForKMemRegion()> &&
        allHigherLevelTablesAreSmallPageMultiples<0>;

    // Total size of all page table levels from `level` down to the leaf level.
    template <size_t level>
    constexpr size_t pageTableStackSize = [] {
        if constexpr (level >= arch::pageTableDescriptor.LEVEL_COUNT) return 0;
        else return pageTableStackSize<level + 1> + sizeof(arch::PageTable<level>);
    }();

    // Space needed for page tables to map one page allocator buffer region.
    // Two complete stacks (upper + lower unaligned portions) share a single root table.
    constexpr size_t requiredTableSizeForPageAllocator =
        2 * pageTableStackSize<pageTableLevelForKMemRegion()> -
        sizeof(arch::PageTable<pageTableLevelForKMemRegion()>);

    struct PageTableInitializationResult {
        phys_addr pageTableAddress;
        size_t mappedAddressStartOffset;
    };

    // Recursively initialize a page table to map a physical memory range.
    //
    // Strategy: Use huge pages for the aligned middle portion, and recurse into subtables
    // for any unaligned head/tail portions that require small pages.
    //
    // Template parameters:
    //   level - The page table level being initialized (e.g., PD=2, PT=3 on x86-64)
    //   upper - If true, this subtable maps the upper portion of a range (populate from bottom up)
    //           If false, this subtable maps the lower portion (populate from top down)
    //
    // Returns:
    //   pageTableAddress          - Physical address of the initialized table
    //   mappedAddressStartOffset  - Offset within the zone where mapped data actually begins
    //                               (accounts for alignment gaps when !upper)
    template <size_t level, bool upper>
    PageTableInitializationResult initializePageTable(
        const virt_addr pageTableBase,   // Virtual address where page table stack is mapped
        const phys_memory_range range,   // Physical memory range to map
        const phys_addr ptPhysicalBase   // Physical address of page table stack base
    ) {
        static_assert(level < arch::pageTableDescriptor.LEVEL_COUNT && level >= pageTableLevelForKMemRegion());

        virt_addr tableStart = pageTableBase;
        if constexpr(level > pageTableLevelForKMemRegion()) {
            tableStart += sizeof(arch::PageTable<pageTableLevelForKMemRegion()>);
            constexpr auto lowerTableStackSize = pageTableStackSize<pageTableLevelForKMemRegion() + 1> - pageTableStackSize<level>;
            tableStart += 2 * lowerTableStackSize;
            if constexpr (upper) {
                tableStart += sizeof(arch::PageTable<level>);
            }
        }
        auto& table = *tableStart.as_ptr<arch::PageTable<level>>();
        size_t offset = 0;

        auto templateEntry = arch::PTE<level>::leafEntry(phys_addr(0ul));
        templateEntry.markPresent();
        templateEntry.enableWrite();
        if constexpr (arch::pageTableDescriptor.levels[level].leafEncoding.properties.canBeGlobal())
            templateEntry.markGlobal();

        if constexpr (level + 1 == arch::pageTableDescriptor.LEVEL_COUNT) {
            assert(range.start.value % arch::smallPageSize == 0 && range.end.value % arch::smallPageSize == 0, "Range must be aligned to a page boundary");
            const auto rangePages = range.getSize() / arch::smallPageSize;
            offset = upper ? 0 : (arch::pageTableDescriptor.entryCount[level] - rangePages) * arch::smallPageSize;
            assert(rangePages <= arch::pageTableDescriptor.entryCount[level], "Range too large for page table");
            for (size_t i = 0; i < rangePages; i++) {
                auto ptIndex = upper ? i : (i + arch::pageTableDescriptor.entryCount[level] - rangePages);
                const auto pageAddr = range.start + i * arch::smallPageSize;
                table[ptIndex] = templateEntry;
                table[ptIndex].setPhysicalAddress(pageAddr);
            }
        }
        else if constexpr(level >= pageTableLevelForKMemRegion()) {
            constexpr size_t pesize = 1ull << arch::pageTableDescriptor.getVirtualAddressBitCount(level + 1);
            if constexpr(level > pageTableLevelForKMemRegion()) {
                if constexpr (upper) {
                    assert(range.start.value % pesize == 0, "Range start must be aligned to a page boundary");
                }
                else {
                    assert(range.end.value % pesize == 0, "Range end must be aligned to a page boundary");
                }
            }
            auto topRange = range;
            auto bottomRange = range;
            bottomRange.end.value = roundUpToNearestMultiple(range.start.value, pesize);
            topRange.start.value = roundDownToNearestMultiple(range.end.value, pesize);
            phys_memory_range middleRange = {bottomRange.end, topRange.start};

            auto subtableTemplate = arch::PTE<level>::subtableEntry(phys_addr(0ul));
            subtableTemplate.markPresent();
            subtableTemplate.enableWrite();

            const bool singleSubtableCase = (middleRange.end.value < middleRange.start.value);

            size_t pageCount = 0;
            if (singleSubtableCase) {
                pageCount = 1;
            }
            else {
                if (topRange.getSize() > 0) pageCount++;
                if (bottomRange.getSize() > 0) pageCount++;
                pageCount += middleRange.getSize() / pesize;
            }

            size_t pteEntryIndex = 0;
            if constexpr (!upper) {
                pteEntryIndex = arch::pageTableDescriptor.entryCount[level] - pageCount;
                offset += pteEntryIndex * pesize;
            }
            if constexpr (arch::pageTableDescriptor.levels[level].subtableEncoding.properties.canBeGlobal())
                subtableTemplate.markGlobal();
            if (singleSubtableCase) {
                auto data = initializePageTable<level + 1, upper>(pageTableBase, range, ptPhysicalBase);
                offset += data.mappedAddressStartOffset;
                auto subtableEntry = subtableTemplate;
                subtableEntry.setPhysicalAddress(data.pageTableAddress);
                table[pteEntryIndex] = subtableEntry;
            }
            else {
                if (bottomRange.getSize() > 0) {
                    auto data = initializePageTable<level + 1, false>(pageTableBase, bottomRange, ptPhysicalBase);
                    offset += data.mappedAddressStartOffset;
                    auto subtableEntry = subtableTemplate;
                    subtableEntry.setPhysicalAddress(data.pageTableAddress);
                    table[pteEntryIndex] = subtableEntry;
                    pteEntryIndex++;
                }
                for (; middleRange.getSize() > 0; middleRange.start += pesize, pteEntryIndex++) {
                    const auto pageAddr = middleRange.start;
                    table[pteEntryIndex] = templateEntry;
                    table[pteEntryIndex].setPhysicalAddress(pageAddr);
                }
                if (topRange.getSize() > 0) {
                    auto data = initializePageTable<level + 1, true>(pageTableBase, topRange, ptPhysicalBase);
                    auto subtableEntry = subtableTemplate;
                    subtableEntry.setPhysicalAddress(data.pageTableAddress);
                    table[pteEntryIndex] = subtableEntry;
                }
            }
        }

        const auto pageTablePaddr = ptPhysicalBase + (tableStart.value - pageTableBase.value);
        return {pageTablePaddr, offset};
    }

} // namespace kernel::mm

#endif //CROCOS_BOOTSTRAPMAPPER_H
