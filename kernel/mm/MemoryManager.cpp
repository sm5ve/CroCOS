//
// Created by Spencer Martin on 2/17/25.
//

#include <mem/mm.h>

#include <kernel.h>
#include <kmemlayout.h>

namespace kernel::mm{
    size_t phys_memory_range::getSize() const {
        return this -> end.value - this -> start.value;
    }

    size_t virt_memory_range::getSize() const {
        return this -> end.value - this -> start.value;
    }

    bool phys_memory_range::contains(const phys_addr addr) const {
        return (addr.value >= this -> start.value) && (addr.value < this -> end.value);
    }

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

    void* mapTemporaryWindow(const phys_addr base) {
        if constexpr (supportsSimpleTemporaryMapping) {
            static arch::PageTable<pageTableLevelForKMemRegion()> temporaryPageTable;
            const auto paddr = early_boot_virt_to_phys(virt_addr(&temporaryPageTable));
            auto newEntry = KMemRegionEntryType::subtableEntry(paddr);
            newEntry.enableWrite();
            newEntry.markPresent();
            constexpr size_t bigPageSize = 1ull << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion() + 1);
            const uint64_t clampedBase = base.value & ~(bigPageSize - 1);
            for (size_t i = 0; i < arch::pageTableDescriptor.entryCount[pageTableLevelForKMemRegion()]; i++) {
                const auto pageAddr = phys_addr(clampedBase + i * bigPageSize);
                temporaryPageTable[i] = arch::PTE<pageTableLevelForKMemRegion()>::leafEntry(pageAddr);
                temporaryPageTable[i].markPresent();
                temporaryPageTable[i].enableWrite();
            }
            virt_addr outputBase = getKernelMemRegionStart(TEMPORARY_AND_PAGE_TABLE_ZONE);
            getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = newEntry;
            arch::flushTLB();
            return outputBase.as_ptr<void>();
        }
        static_assert(supportsSimpleTemporaryMapping, "Temporary mapping not supported on this architecture with the simple mapping construction");
    }

    void unmapTemporaryWindow() {
        getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = {};
        arch::flushTLB();
    }

    void* reservePageAllocatorBufferForRange(const phys_memory_range& range) {
        const auto processor_count = arch::processorCount();
        const auto requiredBufferSize = PageAllocator::requestedBufferSizeForRange(range, processor_count);
        assert(2 * requiredBufferSize <= getKernelMemRegionSize(), "Memory range is too big"); //Conservative check
        (void)requiredBufferSize;
        return nullptr;
    }
}
