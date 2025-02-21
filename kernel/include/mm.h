//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_MM_H
#define CROCOS_MM_H

#include "stddef.h"
#include <lib/TypeTraits.h>
#include <arch/hal.h>
#include <lib/ds/Vector.h>

namespace kernel::mm{
    struct phys_addr {
        uint64_t value;
        constexpr explicit phys_addr(uint64_t v) : value(v) {}
        constexpr explicit phys_addr(void* v) : value((uint64_t)v) {}
    };

    struct virt_addr {
        uint64_t value;
        constexpr explicit virt_addr(uint64_t v) : value(v) {}
        constexpr explicit virt_addr(void* v) : value((uint64_t)v) {}
        template <typename T>
        constexpr T* as_ptr(){return (T*)value;};
    };

    struct phys_memory_range {
        phys_addr start;
        phys_addr end;
        size_t getSize();
    };

    namespace PageAllocator{
#ifdef __x86_64__
        constexpr size_t smallPageSize = 0x1000; //4KiB
        constexpr size_t bigPageSize = 0x200000; //2MiB
        constexpr size_t maxMemorySupported = (1ULL << 48); //256 TiB
#endif

        struct page_allocator_range_info{
            phys_memory_range range;
            //The architecture initialization routine MUST reserve adequately sized buffers for each
            //memory range as specified by requestedBufferSizeForRange. The buffers are expected to be
            //zeroed out.
            void* buffer_start;
        };
        // Calculate the maximum page counts
        constexpr size_t smallPagesPerBigPage = bigPageSize / smallPageSize;
        constexpr size_t bigPagesInMaxMemory = maxMemorySupported / bigPageSize;
        void init(Vector<page_allocator_range_info>& regions, size_t processor_count);
        size_t requestedBufferSizeForRange(mm::phys_memory_range range, size_t processor_count);
    }

    phys_addr virt_to_phys(virt_addr);
    virt_addr phys_to_virt(phys_addr);
}

#endif //CROCOS_MM_H
