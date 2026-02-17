//
// Created by Spencer Martin on 2/14/26.
//


#include <mem/MemTypes.h>

namespace kernel::mm{
    size_t phys_memory_range::getSize() const {
        return this -> end.value - this -> start.value;
    }

    size_t virt_memory_range::getSize() const {
        return this -> end.value - this -> start.value;
    }

    bool phys_memory_range::contains(const phys_addr addr) const {
        return (addr.value >= this -> start.value) && (addr.value < this -> end.value);
    }
}
