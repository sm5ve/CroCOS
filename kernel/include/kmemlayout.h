//
// Created by Spencer Martin on 1/22/26.
//

#ifndef CROCOS_KMEMLAYOUT_H
#define CROCOS_KMEMLAYOUT_H

#include <arch.h>
#include <stdint.h>

extern "C" arch::PageTable<0> bootPageTable;

namespace kernel::mm{
    constexpr size_t MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2 = 28; // 256 MiB
    constexpr size_t KERNEL_ZONE = 0;
    constexpr size_t TEMPORARY_AND_PAGE_TABLE_ZONE = 1;
    constexpr size_t PAGE_ALLOCATOR_ZONE_START = 2;

    [[nodiscard]] constexpr size_t pageTableLevelForKMemRegion(const size_t regionSizeLog2 = MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2) {
        for(size_t i = 0; i < arch::pageTableDescriptor.LEVEL_COUNT; i++) {
            const auto level = arch::pageTableDescriptor.LEVEL_COUNT - i;
            if (arch::pageTableDescriptor.getVirtualAddressBitCount(level) >= regionSizeLog2) {
                return level;
            }
        }
        return -1; //Should not be reached, just here to prevent the compiler from complaining
    }

    [[nodiscard]] constexpr size_t getKernelMemRegionSize() {
        return 1 << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion());
    }

    [[nodiscard]] constexpr virt_addr getKernelMemRegionStart(size_t index) {
        auto address = virt_addr{static_cast<uint64_t>(-(index + 1) * getKernelMemRegionSize())};
        return arch::pageTableDescriptor.canonicalizeVirtualAddress(address);
    }

    constexpr size_t kStart = getKernelMemRegionStart(KERNEL_ZONE).value;

    constexpr virt_addr early_boot_phys_to_virt(phys_addr x){
        return virt_addr(x.value + kStart);
    }

    constexpr phys_addr early_boot_virt_to_phys(virt_addr x){
        return phys_addr(x.value - kStart);
    }

    void unmapIdentity();
    void remapIdentity();
    void* mapTemporaryWindow(phys_addr base);
    void unmapTemporaryWindow();
}

#endif //CROCOS_KMEMLAYOUT_H