//
// Created by Spencer Martin on 1/22/26.
//

#ifndef CROCOS_KMEMLAYOUT_H
#define CROCOS_KMEMLAYOUT_H

#include <arch.h>
#include <stdint.h>

namespace kernel{
    constexpr size_t MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2 = 28; // 256 MiB

    [[nodiscard]] constexpr size_t pageTableLevelForKMemRegion(const size_t regionSizeLog2) {
        for(size_t i = 0; i < arch::pageTableDescriptor.LEVEL_COUNT; i++) {
            const auto level = arch::pageTableDescriptor.LEVEL_COUNT - i;
            if (arch::pageTableDescriptor.getVirtualAddressBitCount(level) >= regionSizeLog2) {
                return level;
            }
        }
        return -1; //Should not be reached, just here to prevent the compiler from complaining
    }

    [[nodiscard]] constexpr size_t getKernelMemRegionSize() {
        return 1 << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion(MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2));
    }

    [[nodiscard]] constexpr mm::virt_addr getKernelMemRegionStart(size_t index) {
        auto address = mm::virt_addr{static_cast<uint64_t>(-(index + 1) * getKernelMemRegionSize())};
        return arch::pageTableDescriptor.canonicalizeVirtualAddress(address);
    }

}

#endif //CROCOS_KMEMLAYOUT_H