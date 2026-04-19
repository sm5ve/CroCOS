//
// Created by Spencer Martin on 2/17/25.
//

#include <mem/mm.h>
#include <mem/PageAllocator.h>

#include <kernel.h>
#include <kmemlayout.h>
#include <mem/NUMA.h>

extern uint32_t phys_end;

static PageAllocatorImpl gPageAllocatorImpl;

namespace kernel::mm{
    template<size_t level>
    void unmapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
        constexpr auto tableEntries = arch::pageTableDescriptor.entryCount[level];
        constexpr auto topEntry = tableEntries - 1;
        if constexpr (level < unmapCeiling - 1) {
            const auto subtablePaddr = pageTable[topEntry].getPhysicalAddress();
            const auto subtable = early_boot_phys_to_virt(subtablePaddr).template as_ptr<arch::PageTable<level + 1>>();
            unmapIdentity<level + 1>(*subtable);
        }
        pageTable[0] = {};
    }

    template<size_t level>
    void remapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
        static_assert(unmapCeiling > level);
        constexpr auto tableEntries = arch::pageTableDescriptor.entryCount[level];
        constexpr auto topEntry = tableEntries - 1;
        if constexpr (level < unmapCeiling - 1) {
            auto subtablePaddr = pageTable[topEntry].getPhysicalAddress();
            auto subtable = early_boot_phys_to_virt(subtablePaddr).template as_ptr<arch::PageTable<level + 1>>();
            remapIdentity<level + 1>(*subtable);
        }
        pageTable[0] = pageTable[topEntry];
    }

    void unmapIdentity() {
        unmapIdentity<0>(bootPageTable);
    }

    void remapIdentity() {
        remapIdentity<0>(bootPageTable);
    }

    using KMemRegionEntryType = arch::PTE<pageTableLevelForKMemRegion() - 1>;

    template<size_t level>
    arch::PageTable<level + 1>& getTopmostSubtable(const arch::PageTable<level>& pageTable) {
        auto paddr = pageTable[arch::pageTableDescriptor.entryCount[level] - 1].getPhysicalAddress();
        return *early_boot_phys_to_virt(paddr).template as_ptr<arch::PageTable<level + 1>>();
    }

    template <size_t desiredLevel, size_t currentLevel>
    arch::PageTable<desiredLevel>& getTopmostTable(arch::PageTable<currentLevel>& current) {
        static_assert(desiredLevel >= currentLevel);
        if constexpr (currentLevel == desiredLevel) {
            return current;
        }
        else {
            return getTopmostTable<desiredLevel, currentLevel + 1>(getTopmostSubtable<currentLevel>(current));
        }
    }

    template <size_t desiredLevel>
    arch::PageTable<desiredLevel>& getTopmostTable() {
        return getTopmostTable<desiredLevel, 0>(bootPageTable);
    }

    KMemRegionEntryType& getPageTableEntryForZone(const size_t zone) {
        const auto zoneIndex = arch::pageTableDescriptor.entryCount[pageTableLevelForKMemRegion() - 1] - zone - 1;
        return getTopmostTable<pageTableLevelForKMemRegion() - 1>()[zoneIndex];
    }

    static_assert(pageTableLevelForKMemRegion() < arch::pageTableDescriptor.LEVEL_COUNT);

    constexpr bool supportsSimpleTemporaryMapping = []{
        return arch::PTE<pageTableLevelForKMemRegion()>::canBeLeaf();
    }();

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

    constexpr bool supportsSimpleBootstrapPageAllocatorMapping = allHigherLevelsCanBeLeaves<pageTableLevelForKMemRegion()> && allHigherLevelTablesAreSmallPageMultiples<0>;
    constexpr size_t temporaryWindowAlign = 1ull << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion() + 1);

    virt_addr mapTemporaryWindow(const phys_addr base) {
        if constexpr (supportsSimpleTemporaryMapping) {
            static arch::PageTable<pageTableLevelForKMemRegion()> temporaryPageTable;
            const auto paddr = early_boot_virt_to_phys(virt_addr(&temporaryPageTable));
            auto newEntry = KMemRegionEntryType::subtableEntry(paddr);
            newEntry.enableWrite();
            newEntry.markPresent();
            const uint64_t clampedBase = base.value & ~(temporaryWindowAlign - 1);
            for (size_t i = 0; i < arch::pageTableDescriptor.entryCount[pageTableLevelForKMemRegion()]; i++) {
                const auto pageAddr = phys_addr(clampedBase + i * temporaryWindowAlign);
                temporaryPageTable[i] = arch::PTE<pageTableLevelForKMemRegion()>::leafEntry(pageAddr);
                temporaryPageTable[i].markPresent();
                temporaryPageTable[i].enableWrite();
            }
            virt_addr outputBase = getKernelMemRegionStart(TEMPORARY_AND_PAGE_TABLE_ZONE);
            getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = newEntry;
            arch::flushTLB();
            return outputBase;
        }
        static_assert(supportsSimpleTemporaryMapping, "Temporary mapping not supported on this architecture with the simple mapping construction");
    }

    void unmapTemporaryWindow() {
        getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = {};
        arch::flushTLB();
    }

    // Recursively computes the total size needed for all page tables from 'level' down to leaf level.
    // For example, on x86-64 starting at PD level: sizeof(PD) + sizeof(PT) + sizeof(small page)
    template <size_t level>
    constexpr size_t pageTableStackSize = [] {
        if constexpr (level >= arch::pageTableDescriptor.LEVEL_COUNT) return 0;
        else return pageTableStackSize<level + 1> + sizeof(arch::PageTable<level>);
    }();

    // Space needed for page tables to map a page allocator buffer.
    // We need two complete stacks (for upper and lower unaligned portions) but only one root table.
    constexpr size_t requiredTableSizeForPageAllocator = 2 * pageTableStackSize<pageTableLevelForKMemRegion()> - sizeof(arch::PageTable<pageTableLevelForKMemRegion()>);

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
    //   pageTableAddress - Physical address of the initialized table
    //   mappedAddressStartOffset - Offset within the zone where mapped data actually begins
    //                               (accounts for alignment gaps when !upper)
    template <size_t level, bool upper>
    PageTableInitializationResult initializePageTable(
        const virt_addr pageTableBase,  // Virtual address where page table stack is mapped
        const phys_memory_range range,        // Physical memory range to map
        const phys_addr ptPhysicalBase  // Physical address of page table stack base
    ) {
        static_assert(level < arch::pageTableDescriptor.LEVEL_COUNT && level >= pageTableLevelForKMemRegion());

        // Navigate to the correct position in the statically allocated page table stack
        virt_addr tableStart = pageTableBase;
        if constexpr(level > pageTableLevelForKMemRegion()) {
            // Skip past the root table
            tableStart += sizeof(arch::PageTable<pageTableLevelForKMemRegion()>);
            // Skip past subtables at levels between root and current level
            // Factor of 2 accounts for both upper and lower subtable stacks
            constexpr auto lowerTableStackSize = pageTableStackSize<pageTableLevelForKMemRegion() + 1> - pageTableStackSize<level>;
            tableStart += 2 * lowerTableStackSize;
            // If this is an upper subtable, skip past the lower subtable at this level
            if constexpr (upper) {
                tableStart += sizeof(arch::PageTable<level>);
            }
        }
        auto& table = *tableStart.as_ptr<arch::PageTable<level>>();
        size_t offset = 0; // Tracks offset from zone start to where actual data begins

        // Create a template PTE for leaf entries (huge pages or small pages)
        auto templateEntry = arch::PTE<level>::leafEntry(phys_addr(0ul));
        templateEntry.markPresent();
        templateEntry.enableWrite();
        if constexpr (arch::pageTableDescriptor.levels[level].leafEncoding.properties.canBeGlobal())
            templateEntry.markGlobal();

        // Base case: We're at the leaf level (page table on x86-64)
        if constexpr (level + 1 == arch::pageTableDescriptor.LEVEL_COUNT) {
            assert(range.start.value % arch::smallPageSize == 0 && range.end.value % arch::smallPageSize == 0, "Range must be aligned to a page boundary");
            const auto rangePages = range.getSize() / arch::smallPageSize;
            // For upper subtables, populate from the start of the table, so the offset is 0
            // For lower subtables, populate up to the end (leaving a gap at the start), so we may have a nonzero offset
            offset = upper ? 0 : (arch::pageTableDescriptor.entryCount[level] - rangePages) * arch::smallPageSize;
            assert(rangePages <= arch::pageTableDescriptor.entryCount[level], "Range too large for page table");
            for (size_t i = 0; i < rangePages; i++) {
                auto ptIndex = upper ? i : (i + arch::pageTableDescriptor.entryCount[level] - rangePages);
                const auto pageAddr = range.start + i * arch::smallPageSize;
                table[ptIndex] = templateEntry;
                table[ptIndex].setPhysicalAddress(pageAddr);
            }
        }
        // Recursive case: Use huge pages where aligned, recurse into subtables for unaligned portions
        else if constexpr(level >= pageTableLevelForKMemRegion()) {
            //Size of region of memory mapped to by a single entry in the current page table level
            constexpr size_t pesize = 1ull << arch::pageTableDescriptor.getVirtualAddressBitCount(level + 1);
            //If we're in the recursive case, we expect one half of the range to be properly aligned
            if constexpr(level > pageTableLevelForKMemRegion()) {
                if constexpr (upper) {
                    assert(range.start.value % pesize == 0, "Range start must be aligned to a page boundary");
                }
                else {
                    assert(range.end.value % pesize == 0, "Range end must be aligned to a page boundary");
                }
            }
            // Partition the range into three parts:
            // bottomRange: unaligned head (needs subtable with small pages)
            // middleRange: aligned middle (can use huge pages)
            // topRange: unaligned tail (needs subtable with small pages)
            auto topRange = range;
            auto bottomRange = range;
            bottomRange.end.value = roundUpToNearestMultiple(range.start.value, pesize);
            topRange.start.value = roundDownToNearestMultiple(range.end.value, pesize);
            phys_memory_range middleRange = {bottomRange.end, topRange.start};

            // Template for subtable entries
            auto subtableTemplate = arch::PTE<level>::subtableEntry(phys_addr(0ul));
            subtableTemplate.markPresent();
            subtableTemplate.enableWrite();

            const bool singleSubtableCase = (middleRange.end.value < middleRange.start.value);

            // Calculate total entries needed at this level
            size_t pageCount = 0;
            if (singleSubtableCase) {
                pageCount = 1;
            }
            else {
                if (topRange.getSize() > 0) pageCount++; // Upper subtable entry
                if (bottomRange.getSize() > 0) pageCount++; //Lower subtable entry
                pageCount += middleRange.getSize() / pesize; //Huge pages
            }

            // Special case: entire range is smaller than one huge page, use single subtable
            size_t pteEntryIndex = 0;
            if constexpr (!upper) {
                pteEntryIndex = arch::pageTableDescriptor.entryCount[level] - pageCount;
                //Hence we get a nonzero offset
                offset += pteEntryIndex * pesize;
            }
            if constexpr (arch::pageTableDescriptor.levels[level].subtableEncoding.properties.canBeGlobal())
                subtableTemplate.setGlobal();
            // If the entire range fits in a single huge page, then we recurse and just add a subtable entry
            if (singleSubtableCase) {
                auto data = initializePageTable<level + 1, upper>(pageTableBase, range, ptPhysicalBase);
                offset += data.mappedAddressStartOffset;
                auto subtableEntry = subtableTemplate;
                subtableEntry.setPhysicalAddress(data.pageTableAddress);
                table[pteEntryIndex] = subtableEntry;
            }
            else{
                // Handle unaligned bottom portion
                if (bottomRange.getSize() > 0) {
                    auto data = initializePageTable<level + 1, false>(pageTableBase, bottomRange, ptPhysicalBase);
                    offset += data.mappedAddressStartOffset;
                    auto subtableEntry = subtableTemplate;
                    subtableEntry.setPhysicalAddress(data.pageTableAddress);
                    table[pteEntryIndex] = subtableEntry;
                    pteEntryIndex++;
                }
                // Map aligned middle portion with huge pages
                for (; middleRange.getSize() > 0; middleRange.start += pesize, pteEntryIndex++) {
                    const auto pageAddr = middleRange.start;
                    table[pteEntryIndex] = templateEntry;
                    table[pteEntryIndex].setPhysicalAddress(pageAddr);
                }
                // Handle unaligned top portion
                if (topRange.getSize() > 0) {
                    auto data = initializePageTable<level + 1, true>(pageTableBase, topRange, ptPhysicalBase);
                    auto subtableEntry = subtableTemplate;
                    subtableEntry.setPhysicalAddress(data.pageTableAddress);
                    table[pteEntryIndex] = subtableEntry;
                }
            }
        }

        // Convert virtual address of this table back to physical
        const auto pageTablePaddr = ptPhysicalBase + (tableStart.value - pageTableBase.value);

        return {pageTablePaddr, offset};
    }

    // Shared zone counter across all reservePageAllocatorBufferForRange overloads.
    static size_t gMappedPageAllocatorBuffers = 0;

    // Reserve and map a page allocator buffer for a physical memory range.
    //
    // This function carves out space at the top of the physical range for:
    // 1. Page table structures needed to map the buffer
    // 2. The buffer itself (requiredBufferSize bytes, rounded up to page granularity)
    //
    // The function modifies the input range to mark the reserved space as used,
    // sets up the page tables to map the buffer into the next available page
    // allocator zone, and returns a virtual pointer to the mapped buffer.
    //
    // Returns: Virtual address of the mapped buffer
    void* reservePageAllocatorBufferForRange(phys_memory_range& range, size_t requiredBufferSize) {
        if constexpr (supportsSimpleBootstrapPageAllocatorMapping) {
            //Align range to page boundaries
            range.end &= ~(arch::smallPageSize - 1);
            range.start.value = roundUpToNearestMultiple(range.start.value, arch::smallPageSize);

            // Reserve space at the top of the range for page table structures
            range.end -= requiredTableSizeForPageAllocator;
            const auto ptPhysicalBase = range.end;

            // Map the page table structures via the temporary window
            // (they need to be accessible to initialize them)
            phys_addr temporaryMapWindowBase = range.end & ~(temporaryWindowAlign - 1);
            size_t offset = range.end.value - temporaryMapWindowBase.value;
            virt_addr pageTableBase = mapTemporaryWindow(temporaryMapWindowBase) + offset;
            //Clear out the page tables
            memset(pageTableBase.as_ptr<void>(), 0, requiredTableSizeForPageAllocator);

            // Round buffer size up to page granularity, then sanity check
            const size_t alignedBufferSize = roundUpToNearestMultiple(requiredBufferSize, arch::smallPageSize);
            assert(2 * alignedBufferSize <= getKernelMemRegionSize(), "Memory range is too big"); //Conservative check

            // Reserve space for the buffer itself (also at top of range, before page tables)
            phys_memory_range bufferRange(range.end - alignedBufferSize, range.end);
            range.end -= alignedBufferSize;

            // Initialize page tables to map the buffer into a page allocator zone
            auto data = initializePageTable<pageTableLevelForKMemRegion(), true>
                (pageTableBase, bufferRange, ptPhysicalBase);

            // Install the initialized page table into the kernel's page table hierarchy
            // at the next available page allocator zone
            auto ptentry = KMemRegionEntryType::subtableEntry(data.pageTableAddress);
            ptentry.markPresent();
            ptentry.enableWrite();
            getPageTableEntryForZone(PAGE_ALLOCATOR_ZONE_START + gMappedPageAllocatorBuffers) = ptentry;

            // Calculate the virtual address where the buffer is now mapped
            auto mappedAddress = getKernelMemRegionStart(PAGE_ALLOCATOR_ZONE_START + gMappedPageAllocatorBuffers) + data.mappedAddressStartOffset;
            gMappedPageAllocatorBuffers++; // Advance to next zone for the next buffer
            arch::flushTLB();
            return mappedAddress.as_ptr<void>();
        }
        static_assert(supportsSimpleBootstrapPageAllocatorMapping, "Page allocator buffer mapping not supported on this architecture with the simple mapping construction");
    }

    bool initPageAllocator() {
        const numa::NUMATopology* topology = numa::getCurrentTopology();

        // Collect usable physical ranges from the firmware memory map.
        Vector<phys_memory_range> usableRanges;
        for (auto entry : arch::getMemoryMap()) {
            if (entry.type == arch::USABLE && entry.range.getSize() > arch::bigPageSize * 2) {
                usableRanges.push(entry.range);
            }
        }

        // Per-processor LocalPool pointer table (indexed by ProcessorID).
        static LocalPool* localPools[arch::MAX_PROCESSOR_COUNT] = {};

        Vector<NUMAPool*> numaPools;
        NUMAPool* unownedPool = nullptr;
        size_t processorCount = arch::processorCount();
        const kernel::numa::NUMAPolicy* allocPolicy = nullptr;

        if (topology != nullptr) {
            // NUMA-aware path: partition ranges by proximity domain.
            numa::NUMAMemoryPartition partition =
                numa::partitionMemoryByDomain(usableRanges, processorCount, *topology);
            processorCount = partition.processorDomain.size();

            bool anyDomainHasMemory = false;
            for (size_t d = 0; d < partition.rangesPerDomain.size(); d++) {
                if (!partition.rangesPerDomain[d].empty()) { anyDomainHasMemory = true; break; }
            }

            if (!anyDomainHasMemory) {
                // Topology exists but has no SRAT memory regions (e.g., trivial single-domain
                // on QEMU): all usable memory ended up in unownedRanges. Fall back to a single
                // pool with no NUMA policy so createPageAllocator's invariants hold.
                auto& ranges = partition.unownedRanges;
                BootstrapAllocator measuringAlloc;
                createNumaPool(measuringAlloc, ranges);
                for (size_t cpu = 0; cpu < processorCount; cpu++)
                    createLocalPool(measuringAlloc, nullptr);
                phys_memory_range* largestRange = &ranges[0];
                for (size_t i = 1; i < ranges.size(); i++)
                    if (ranges[i].getSize() > largestRange->getSize()) largestRange = &ranges[i];
                void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
                BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                NUMAPool* singlePool = createNumaPool(realAlloc, ranges);
                numaPools.push(singlePool);
                for (size_t cpu = 0; cpu < processorCount; cpu++)
                    localPools[cpu] = createLocalPool(realAlloc, nullptr, singlePool);
            } else {
                for (size_t domainIdx = 0; domainIdx < partition.rangesPerDomain.size(); domainIdx++) {
                    auto& ranges = partition.rangesPerDomain[domainIdx];
                    if (ranges.empty()) {
                        numaPools.push(nullptr);
                        continue;
                    }
                    const numa::DomainID domainId{static_cast<uint16_t>(domainIdx)};

                    // ---- Measure required memory ----
                    BootstrapAllocator measuringAlloc;
                    createNumaPool(measuringAlloc, ranges, domainId);
                    for (size_t cpu = 0; cpu < processorCount; cpu++) {
                        if (partition.processorDomain[cpu] == domainId)
                            createLocalPool(measuringAlloc, topology);
                    }

                    // ---- Carve out buffer from the largest range ----
                    phys_memory_range* largestRange = &ranges[0];
                    for (size_t i = 1; i < ranges.size(); i++)
                        if (ranges[i].getSize() > largestRange->getSize()) largestRange = &ranges[i];
                    void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());

                    // ---- Construct pools ----
                    BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                    NUMAPool* domainPool = createNumaPool(realAlloc, ranges, domainId);
                    numaPools.push(domainPool);
                    for (size_t cpu = 0; cpu < processorCount; cpu++) {
                        if (partition.processorDomain[cpu] == domainId)
                            localPools[cpu] = createLocalPool(realAlloc, topology, domainPool);
                    }
                }

                allocPolicy = &kernel::numa::numaPolicy();

                // ---- Unowned ranges (no NUMA affinity) ----
                if (!partition.unownedRanges.empty()) {
                    BootstrapAllocator measuringAlloc;
                    createNumaPool(measuringAlloc, partition.unownedRanges);
                    phys_memory_range* largestRange = &partition.unownedRanges[0];
                    for (size_t i = 1; i < partition.unownedRanges.size(); i++)
                        if (partition.unownedRanges[i].getSize() > largestRange->getSize())
                            largestRange = &partition.unownedRanges[i];
                    void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
                    BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                    unownedPool = createNumaPool(realAlloc, partition.unownedRanges);
                }
            }
        } else {
            // Non-NUMA path: single pool for all memory.
            BootstrapAllocator measuringAlloc;
            createNumaPool(measuringAlloc, usableRanges);
            for (size_t cpu = 0; cpu < processorCount; cpu++)
                createLocalPool(measuringAlloc, topology);
            phys_memory_range* largestRange = &usableRanges[0];
            for (size_t i = 1; i < usableRanges.size(); i++)
                if (usableRanges[i].getSize() > largestRange->getSize()) largestRange = &usableRanges[i];
            void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
            BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
            NUMAPool* singlePool = createNumaPool(realAlloc, usableRanges);
            numaPools.push(singlePool);
            for (size_t cpu = 0; cpu < processorCount; cpu++)
                localPools[cpu] = createLocalPool(realAlloc, topology, singlePool);
        }

        unmapTemporaryWindow();

        gPageAllocatorImpl = createPageAllocator(move(numaPools), localPools, processorCount, unownedPool, allocPolicy);
        gPageAllocator = &gPageAllocatorImpl;
        klog() << "[PA] initPageAllocator complete, gPageAllocator=" << (void*)gPageAllocator << " localPools[0]=" << (void*)localPools[0] << "\n";

        // Reserve the kernel image so it can never be handed out as free memory.
        phys_memory_range kernelRange{.start = phys_addr(nullptr), .end = phys_addr(&phys_end)};
        klog() << "[PA] kernelRange.end = " << (void*)kernelRange.end.value << "\n";
        gPageAllocator->reserveRange(kernelRange);
        klog() << "[PA] reserveRange done\n";

        return true;
    }
}
