//
// Created by Spencer Martin on 2/14/26.
//


#include <mem/MemTypes.h>

namespace kernel::mm{
    size_t phys_memory_range::getSize() const {
        return isEmpty() ? 0 : end.value - start.value;
    }

    bool phys_memory_range::isEmpty() const {
        return end.value <= start.value;
    }

    bool phys_memory_range::contains(const phys_addr addr) const {
        return !isEmpty() && (addr.value >= start.value) && (addr.value < end.value);
    }

    phys_memory_range phys_memory_range::intersect(const phys_memory_range other) const {
        phys_addr lo{start.value > other.start.value ? start.value : other.start.value};
        phys_addr hi{end.value   < other.end.value   ? end.value   : other.end.value};
        return {lo, hi};
    }

    size_t virt_memory_range::getSize() const {
        return isEmpty() ? 0 : end.value - start.value;
    }

    bool virt_memory_range::isEmpty() const {
        return end.value <= start.value;
    }

    bool virt_memory_range::contains(const virt_addr addr) const {
        return !isEmpty() && (addr.value >= start.value) && (addr.value < end.value);
    }

    virt_memory_range virt_memory_range::intersect(const virt_memory_range other) const {
        virt_addr lo{start.value > other.start.value ? start.value : other.start.value};
        virt_addr hi{end.value   < other.end.value   ? end.value   : other.end.value};
        return {lo, hi};
    }
}
