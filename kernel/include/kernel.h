//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_KERNEL_H
#define CROCOS_KERNEL_H

#include <core/PrintStream.h>
#include "stddef.h"
#include <core/utility.h>

namespace kernel{
    extern PrintStream& klog;
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

    class PrintStream;
}

inline kernel::PrintStream& operator<<(kernel::PrintStream& ps, kernel::mm::phys_addr paddr){
    return ps << "phys_addr(" << (void*)paddr.value << ")";
}

inline kernel::PrintStream& operator<<(kernel::PrintStream& ps, kernel::mm::virt_addr vaddr){
    return ps << "virt_addr(" << (void*)vaddr.value << ")";
}

extern "C" void* memset(void* dest, int value, size_t len);
extern "C" void* memswap(void* dest, const void* src, size_t len);
extern "C" void* memcpy(void* dest, const void* src, size_t len);

//Kinda hacky - what appears to be a call to the constructor name(__VA_ARGS__) does nothing
//but it tricks the compiler into going forward with compilation. The name##_init() is what actually
//calls the constructor.
//This is a seemingly necessary hack to prevent having to recompile crtstart/end with the kernel memory model
//and linking it in with the kernel as suggested by https://forum.osdev.org/viewtopic.php?t=28066
#define WITH_GLOBAL_CONSTRUCTOR(Type, name, ...)                                  \
    __attribute__((used)) static Type name(__VA_ARGS__);                          \
    static void name##_init() {                                                   \
        new (&name) Type(__VA_ARGS__);                                            \
    }                                                                             \
    static void (*name##_ctor)(void) __attribute__((used, section(".init_array"))) = name##_init;

#endif //CROCOS_KERNEL_H
