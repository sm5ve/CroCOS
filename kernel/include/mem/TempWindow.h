//
// Created by Spencer Martin on 4/27/26.
//

#ifndef CROCOS_TEMPWINDOW_H
#define CROCOS_TEMPWINDOW_H

#include <arch.h>
#include <kmemlayout.h>

namespace kernel::mm {

namespace detail {
    // Backing table for the temporary mapping window. Only one TempWindow may be active at a time.
    // Slot 0 is a self-referential subtable entry; slots 1..N are leaf entries for mapped pages.
    inline arch::PageTable<pageTableLevelForKMemRegion()> tempWindowTable{};
}

// Provides temporary virtual access to arbitrary physical pages during bootstrap.
//
// sizeof(T) must equal arch::smallPageSize (typically T is a page table type).
// temp[i] maps physBase + i*smallPageSize and returns T&.
// All mapped slots remain live until the TempWindow is destroyed.
//
// Uses the recursive page table trick: tempWindowTable[0] is a self-referential subtable
// entry, so zoneBase + 0 maps tempWindowTable itself. Leaf entries for temp[i] are written
// into tempWindowTable[i+1] through this recursive mapping; no second hardware table level
// is required, making the design portable across any architecture satisfying
// arch::recursivePageTablesSupported.
//
// NOT safe under concurrency. References returned by operator[] are only valid
// while no subsequent call remaps the same slot index. Do not retain raw pointers.
template <typename T>
struct TempWindow {
    static_assert(sizeof(T) == arch::smallPageSize,
        "TempWindow<T> requires sizeof(T) == arch::smallPageSize");
    static_assert(arch::PTE<pageTableLevelForKMemRegion()>::canBeSubtable(),
        "TempWindow requires the kernel memory region level to support subtable entries");
    static_assert(arch::recursivePageTablesSupported,
        "TempWindow requires recursive page table support");

    explicit TempWindow(phys_addr base) : physBase(base) {
        using ZoneEntry  = arch::PTE<pageTableLevelForKMemRegion() - 1>;
        using TableEntry = arch::PTE<pageTableLevelForKMemRegion()>;

        const auto tablePhysAddr = early_boot_virt_to_phys(virt_addr(&detail::tempWindowTable));

        // Slot 0: self-referential — accessing zoneBase+0 maps tempWindowTable itself as a page,
        // allowing leaf entries at slots 1..N to be updated without early_boot_phys_to_virt.
        auto selfRef = TableEntry::subtableEntry(tablePhysAddr);
        selfRef.markPresent();
        selfRef.enableWrite();
        detail::tempWindowTable[0] = selfRef;

        auto zoneEntry = ZoneEntry::subtableEntry(tablePhysAddr);
        zoneEntry.markPresent();
        zoneEntry.enableWrite();
        getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = zoneEntry;
        // No TLB flush needed — installing new entries for previously-unmapped addresses.
    }

    ~TempWindow() {
        // Clear the table directly (it lives in the kernel image, always accessible).
        for (size_t i = 0; i < arch::pageTableDescriptor.entryCount[pageTableLevelForKMemRegion()]; i++) {
            constexpr virt_addr zoneBase = getKernelMemRegionStart(TEMPORARY_AND_PAGE_TABLE_ZONE);
            if (detail::tempWindowTable[i].isPresent()) {
                arch::invlpg(zoneBase + (i + 1) * arch::smallPageSize);
            }
            detail::tempWindowTable[i] = {};
        }
        getPageTableEntryForZone(TEMPORARY_AND_PAGE_TABLE_ZONE) = {};
    }

    TempWindow(const TempWindow&) = delete;
    TempWindow& operator=(const TempWindow&) = delete;

    T& operator[](size_t i) {
        // Leaf entries are interpreted at the bottom level of the page table hierarchy because
        // the recursive mapping walks tempWindowTable as if it were a leaf-level table.
        // supportsRecursivePageTables guarantees all subtable encodings match this leaf encoding.
        using LeafEntry = arch::PTE<arch::pageTableDescriptor.LEVEL_COUNT - 1>;
        const phys_addr target{physBase.value + i * arch::smallPageSize};
        constexpr virt_addr zoneBase = getKernelMemRegionStart(TEMPORARY_AND_PAGE_TABLE_ZONE);

        auto expected = LeafEntry::leafEntry(target);
        expected.markPresent();
        expected.enableWrite();

        // zoneBase+0 maps tempWindowTable, so (LeafEntry*)zoneBase)[i+1] == tempWindowTable[i+1].
        auto& entrySlot = reinterpret_cast<LeafEntry*>(zoneBase.value)[i + 1];
        if (!(entrySlot == expected)) {
            entrySlot = expected;
            arch::invlpg(zoneBase + (i + 1) * arch::smallPageSize);
        }

        return *reinterpret_cast<T*>((zoneBase + (i + 1) * arch::smallPageSize).value);
    }

    T& operator*() { return (*this)[0]; }

    // Returns the virtual address of temp[0].
    [[nodiscard]] virt_addr virtualBase() const {
        return getKernelMemRegionStart(TEMPORARY_AND_PAGE_TABLE_ZONE) + arch::smallPageSize;
    }

private:
    phys_addr physBase;
};

} // namespace kernel::mm

#endif //CROCOS_TEMPWINDOW_H
