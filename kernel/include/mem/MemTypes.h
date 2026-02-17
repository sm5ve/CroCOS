//
// Created by Spencer Martin on 2/14/26.
//

#ifndef CROCOS_MEMTYPES_H
#define CROCOS_MEMTYPES_H

#include <stdint.h>
#include <stddef.h>
#include <core/PrintStream.h>

namespace kernel::mm{
    struct phys_addr {
        uint64_t value;
        constexpr explicit phys_addr(uint64_t v) : value(v) {}
        constexpr explicit phys_addr() : value(0) {}
        explicit phys_addr(void* v) : value((uint64_t)v) {}
        phys_addr operator+(const size_t offset) const {return phys_addr(value + offset);}
        phys_addr operator-(const size_t offset) const {return phys_addr(value - offset);}
        phys_addr& operator+=(const size_t offset) {value += offset; return *this;}
        phys_addr& operator-=(const size_t offset) {value -= offset; return *this;}
        phys_addr operator&(const size_t mask) const {return phys_addr(value & mask);}
        phys_addr& operator&=(const size_t mask) {value &= mask; return *this;}
        bool operator==(const phys_addr & other) const {return value == other.value;}
    };

    struct virt_addr {
        uint64_t value;
        constexpr explicit virt_addr(uint64_t v) : value(v) {}
        constexpr explicit virt_addr() : value(0) {}
        explicit virt_addr(void* v) : value((uint64_t)v) {}
        template <typename T>
        constexpr T* as_ptr(){return (T*)value;};
        virt_addr operator+(const size_t offset) const {return virt_addr(value + offset);}
        virt_addr operator-(const size_t offset) const {return virt_addr(value - offset);}
        virt_addr& operator+=(const size_t offset) {value += offset; return *this;}
        virt_addr& operator-=(const size_t offset) {value -= offset; return *this;}
        bool operator==(const virt_addr & other) const {return value == other.value;}
    };

    struct phys_memory_range {
        phys_addr start;
        phys_addr end;
        [[nodiscard]] size_t getSize() const;
        [[nodiscard]] bool contains(phys_addr) const;

        bool operator==(const phys_memory_range & other) const {return start == other.start && end == other.end;};
    };

    struct virt_memory_range {
        virt_addr start;
        virt_addr end;
        [[nodiscard]] size_t getSize() const;
    };

    enum class PageMappingPermissions : uint8_t {
        READ  = 1 << 0,
        WRITE = 1 << 1,
        EXEC  = 1 << 2
    };

    enum class PageMappingCacheType {
        FULLY_CACHED,
        FULLY_UNCACHED,
        WRITE_THROUGH,
        WRITE_COMBINE
    };

    enum class PageSize{
        BIG,
        SMALL
    };
}

inline Core::PrintStream& operator<<(Core::PrintStream& ps, kernel::mm::phys_addr paddr){
    return ps << "phys_addr(" << (void*)paddr.value << ")";
}

inline Core::PrintStream& operator<<(Core::PrintStream& ps, kernel::mm::virt_addr vaddr){
    return ps << "virt_addr(" << (void*)vaddr.value << ")";
}

#endif //CROCOS_MEMTYPES_H