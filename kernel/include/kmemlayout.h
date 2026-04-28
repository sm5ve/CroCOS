//
// Created by Spencer Martin on 1/22/26.
//

#ifndef CROCOS_KMEMLAYOUT_H
#define CROCOS_KMEMLAYOUT_H

#include <arch.h>
#include <assert.h>
#include <stdint.h>

extern "C" arch::PageTable<0> bootPageTable;

namespace kernel::mm{
    constexpr size_t MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2 = 28; // 256 MiB

    // Kernel Virtual Address Space Layout
    //
    // PML4[511] -> PDPT: kernel zones, each getKernelMemRegionSize() bytes (1 GiB default)
    //   PDPT[511] = zone 0  (KERNEL_ZONE)                 kernel image
    //   PDPT[510] = zone 1  (TEMPORARY_AND_PAGE_TABLE_ZONE) bootstrap-only temp mapping
    //   PDPT[509] = zone 2+ (PAGE_ALLOCATOR_ZONE_START)   page allocator buffers (one per domain)
    //
    // PML4[VMM_SUBSTRATE_ROOT_INDEX] -> subtable: VMSubstrate internal structures (512 GiB)
    constexpr size_t KERNEL_ZONE = 0;
    constexpr size_t TEMPORARY_AND_PAGE_TABLE_ZONE = 1;
    constexpr size_t PAGE_ALLOCATOR_ZONE_START = 2;

    // Root page table index for the VMSubstrate's region, one slot below the kernel zone (on AMD64: PML4[510]).
    constexpr size_t VMM_SUBSTRATE_ROOT_INDEX = arch::pageTableDescriptor.entryCount[0] - 2;

    [[nodiscard]] constexpr size_t pageTableLevelForKMemRegion(const size_t regionSizeLog2 = MINIMUM_KERNEL_MEM_REGION_SIZE_LOG2) {
        for(size_t i = 0; i < arch::pageTableDescriptor.LEVEL_COUNT; i++) {
            const auto level = arch::pageTableDescriptor.LEVEL_COUNT - i;
            if (arch::pageTableDescriptor.getVirtualAddressBitCount(level) >= regionSizeLog2) {
                return level;
            }
        }
        return SIZE_MAX; //Should not be reached, just here to prevent the compiler from complaining
    }

    [[nodiscard]] constexpr size_t getKernelMemRegionSize() {
        return 1 << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion());
    }

    [[nodiscard]] constexpr virt_addr getKernelMemRegionStart(size_t index) {
        auto address = virt_addr{static_cast<uint64_t>(-(index + 1) * getKernelMemRegionSize())};
        return arch::pageTableDescriptor.canonicalizeVirtualAddress(address);
    }

    [[nodiscard]] constexpr virt_memory_range getZoneVirtualRange(size_t zone) {
        return virt_memory_range{getKernelMemRegionStart(zone),
                                 getKernelMemRegionStart(zone) + getKernelMemRegionSize()};
    }

    constexpr size_t kStart = getKernelMemRegionStart(KERNEL_ZONE).value;

    // Set to true by the init framework at the end of the memory_management phase.
    // Both early_boot translation functions assert this flag is not set.
    inline bool earlyBootMappingExpired = false;

    inline virt_addr early_boot_phys_to_virt(phys_addr x){
        assert(!earlyBootMappingExpired, "early_boot_phys_to_virt called after memory_management phase");
        return virt_addr(x.value + kStart);
    }

    inline phys_addr early_boot_virt_to_phys(virt_addr x){
        assert(!earlyBootMappingExpired, "early_boot_virt_to_phys called after memory_management phase");
        return phys_addr(x.value - kStart);
    }

    // Returns the page table entry (at the kernel memory region level) that controls the
    // virtual mapping for the given zone index.
    arch::PTE<pageTableLevelForKMemRegion() - 1>& getPageTableEntryForZone(size_t zone);

    void unmapIdentity();
    void remapIdentity();

    void* reservePageAllocatorBufferForRange(phys_memory_range& range, size_t requiredBufferSize);
}

#endif //CROCOS_KMEMLAYOUT_H