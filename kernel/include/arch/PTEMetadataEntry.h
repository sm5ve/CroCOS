//
// Created by Spencer Martin on 5/2/26.
//

#ifndef CROCOS_PTEMETADATAENTRY_H
#define CROCOS_PTEMETADATAENTRY_H

#include <arch/PageTableSpecification.h>
#include <core/atomic.h>

namespace arch {

namespace detail {

struct FieldLayout {
    size_t bitOffset;
    constexpr size_t byteOffset() const { return bitOffset / 8; }
};

struct PTEMetadataLayout {
    FieldLayout u16;
    FieldLayout u32;
};

// Returns the first `fieldWidth`-bit-aligned bit offset within `entryWidth` that:
//   - does not contain `presentBit`
//   - does not overlap [blockedStart, blockedStart+blockedWidth)
// Returns SIZE_MAX if no valid placement exists.
consteval size_t findMetadataFieldOffset(size_t presentBit, size_t fieldWidth,
                                         size_t entryWidth,
                                         size_t blockedStart, size_t blockedWidth) {
    for (size_t offset = 0; offset + fieldWidth <= entryWidth; offset += fieldWidth) {
        bool overlapsPresent = (presentBit >= offset && presentBit < offset + fieldWidth);
        bool overlapsBlocked = blockedWidth > 0 &&
            !(offset >= blockedStart + blockedWidth || offset + fieldWidth <= blockedStart);
        if (!overlapsPresent && !overlapsBlocked)
            return offset;
    }
    return SIZE_MAX;
}

consteval PTEMetadataLayout computeMetadataLayout(size_t presentBit, size_t entryWidth) {
    const size_t u16Offset = findMetadataFieldOffset(presentBit, 16, entryWidth, 0, 0);
    const size_t u32Offset = findMetadataFieldOffset(presentBit, 32, entryWidth, u16Offset, 16);
    return {{u16Offset}, {u32Offset}};
}

} // namespace detail

// Wraps a PageTableEntry<encoding> as a not-present metadata slot, packing a uint16_t and
// uint32_t into naturally-aligned positions within the entry's bits.  Placement is computed
// at compile time from the encoding's present-bit position so the two fields never alias it.
//
// For AMD64 (presentBit=0, entryWidth=64):
//   bits  0-15: unused (bit 0 = present, kept clear)
//   bits 16-31: uint16_t  (byte offset 2)
//   bits 32-63: uint32_t  (byte offset 4)
template <PageTableLevelDescriptor encoding>
struct PTEMetadataEntry {
    static constexpr auto layout = detail::computeMetadataLayout(encoding.present, encoding.entryWidth);

    static_assert(encoding.entryWidth == 64, "PTEMetadataEntry requires a 64-bit PTE");
    static_assert(layout.u16.bitOffset != SIZE_MAX, "No valid uint16 placement in metadata PTE");
    static_assert(layout.u32.bitOffset != SIZE_MAX, "No valid uint32 placement in metadata PTE");

    PageTableEntry<encoding> entry{};  // present bit always clear

    [[nodiscard]] uint16_t& u16() {
        return *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(&entry.data) + layout.u16.byteOffset());
    }
    [[nodiscard]] const uint16_t& u16() const {
        return *reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(&entry.data) + layout.u16.byteOffset());
    }

    [[nodiscard]] uint32_t& u32() {
        return *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(&entry.data) + layout.u32.byteOffset());
    }
    [[nodiscard]] const uint32_t& u32() const {
        return *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(&entry.data) + layout.u32.byteOffset());
    }

    // Atomic accessors reinterpret the aligned sub-integer storage as Atomic<T>.
    // Atomic<T> is standard-layout with its value at offset 0, so this aliasing is safe
    // provided the byte offset is naturally aligned — which the layout algorithm guarantees.
    [[nodiscard]] Atomic<uint16_t>& atomicU16() {
        return *reinterpret_cast<Atomic<uint16_t>*>(
            reinterpret_cast<uint8_t*>(&entry.data) + layout.u16.byteOffset());
    }

    [[nodiscard]] Atomic<uint32_t>& atomicU32() {
        return *reinterpret_cast<Atomic<uint32_t>*>(
            reinterpret_cast<uint8_t*>(&entry.data) + layout.u32.byteOffset());
    }
};

} // namespace arch

#endif //CROCOS_PTEMETADATAENTRY_H
