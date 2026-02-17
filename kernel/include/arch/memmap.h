//
// Created by Spencer Martin on 1/25/26.
//

#ifndef CROCOS_MEMMAP_H
#define CROCOS_MEMMAP_H

#include <kernel.h>
#include <core/Iterator.h>
#include <mem/MemTypes.h>

namespace arch{
    enum MemoryMapEntryType : uint8_t {
        USABLE = 0,
        RESERVED = 1,
        ACPI_RECLAIMABLE = 2,
        ACPI_NVS = 3,
        BAD = 4,
        UNKNOWN = 0xff
    };

    struct MemoryMapEntry{
        kernel::mm::phys_memory_range range;
        MemoryMapEntryType type;
    };

    template <typename Iterator>
    concept MemoryMapIterator = requires(Iterator t, const Iterator s, const Iterator r) {
        { *t } -> convertible_to<MemoryMapEntry>;
        { ++t } -> IsSame<Iterator&>;
        { r != s } -> convertible_to<bool>;
    };
}

#endif //CROCOS_MEMMAP_H