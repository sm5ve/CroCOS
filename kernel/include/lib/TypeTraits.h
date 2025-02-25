//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_TYPETRAITS_H
#define CROCOS_TYPETRAITS_H
#include "stdint.h"

template <typename T>
struct is_void {
    static const bool value = false;
};

template <>
struct is_void<void> {
    static const bool value = true;
};

constexpr size_t RequiredBits(size_t value) {
    size_t bits = 0;
    while (value > 0) {
        value >>= 1;
        bits++;
    }
    return bits;
}

// Template to choose the smallest unsigned integer type
template <size_t Bits>
struct SmallestUInt;

template <> struct SmallestUInt<0> { using Type = uint8_t; };
template <> struct SmallestUInt<1> { using Type = uint8_t; };
template <> struct SmallestUInt<8> { using Type = uint8_t; };
template <> struct SmallestUInt<16> { using Type = uint16_t; };
template <> struct SmallestUInt<32> { using Type = uint32_t; };
template <> struct SmallestUInt<64> { using Type = uint64_t; };

// Helper type alias
template <size_t Bits>
using SmallestUInt_t = typename SmallestUInt<
        (Bits <= 8) ? 8 :
        (Bits <= 16) ? 16 :
        (Bits <= 32) ? 32 :
        64
        >::Type;

template <typename T>
struct is_trivially_copyable {
    static constexpr bool value = __is_trivially_copyable(T);
};

template <typename T>
inline constexpr bool is_trivially_copyable_v = is_trivially_copyable<T>::value;
#endif //CROCOS_TYPETRAITS_H
