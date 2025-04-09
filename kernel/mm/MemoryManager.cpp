//
// Created by Spencer Martin on 2/17/25.
//

#include <mm.h>
#include <kconfig.h>

#include <kernel.h>

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
}