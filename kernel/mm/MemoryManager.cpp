//
// Created by Spencer Martin on 2/17/25.
//

#include <../include/mem/mm.h>
#include <kconfig.h>

#include <kernel.h>
#include <kmemlayout.h>

namespace kernel::mm{
    size_t phys_memory_range::getSize() {
        return this -> end.value - this -> start.value;
    }

    size_t virt_memory_range::getSize() {
        return this -> end.value - this -> start.value;
    }

    bool phys_memory_range::contains(kernel::mm::phys_addr addr) {
        return (addr.value >= this -> start.value) && (addr.value < this -> end.value);
    }

    template<size_t level>
    void unmapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
        constexpr auto tableEntries = arch::pageTableDescriptor.entryCount[level];
        constexpr auto topEntry = tableEntries - 1;
        if constexpr (level < unmapCeiling - 1) {
            auto subtablePaddr = pageTable[topEntry].getPhysicalAddress();
            auto subtable = early_boot_phys_to_virt(subtablePaddr).template as_ptr<arch::PageTable<level + 1>>();
            unmapIdentity<level + 1>(*subtable);
        }
        pageTable[0] = {};
    }

    template<size_t level>
    void remapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
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
}
