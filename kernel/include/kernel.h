//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_KERNEL_H
#define CROCOS_KERNEL_H

#include <lib/PrintStream.h>
#include "stddef.h"
#include <utility.h>

namespace kernel{
    extern PrintStream& DbgOut;
    void* kmalloc(size_t size, std::align_val_t = std::align_val_t{1});
    void kfree(void* ptr);

    namespace mm{
        struct phys_addr {
            uint64_t value;
            constexpr explicit phys_addr(uint64_t v) : value(v) {}
            constexpr explicit phys_addr() : value(0) {}
            explicit phys_addr(void* v) : value((uint64_t)v) {}
        };

        struct virt_addr {
            uint64_t value;
            constexpr explicit virt_addr(uint64_t v) : value(v) {}
            constexpr explicit virt_addr() : value(0) {}
            explicit virt_addr(void* v) : value((uint64_t)v) {}
            template <typename T>
            constexpr T* as_ptr(){return (T*)value;};
        };

        enum PageMappingPermissions : uint8_t {
            READ  = 1 << 0,
            WRITE = 1 << 1,
            EXEC  = 1 << 2
        };

        enum PageMappingCacheType {
            FULLY_CACHED,
            FULLY_UNCACHED,
            WRITE_THROUGH,
            WRITE_COMBINE
        };

        enum PageSize{
            BIG,
            SMALL
        };
    }
}

extern "C" void* memset(void* dest, int value, size_t len);
extern "C" void* memswap(void* dest, const void* src, size_t len);
extern "C" void* memcpy(void* dest, const void* src, size_t len);

#endif //CROCOS_KERNEL_H
