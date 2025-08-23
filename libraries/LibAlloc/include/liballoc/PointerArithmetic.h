//
// Created by Spencer Martin on 8/1/25.
//

#ifndef POINTERARITHMETIC_H
#define POINTERARITHMETIC_H

#include <stddef.h>
#include <stdint.h>

template <bool Power2Alignment>
constexpr uintptr_t alignDown(uintptr_t addr, const size_t alignment) {
    if (Power2Alignment) {
        addr &= ~(alignment - 1);
    }
    else {
        addr -= addr % alignment;
    }
    return addr;
}

template <bool Power2Alignment>
constexpr uintptr_t alignUp(const uintptr_t addr, const size_t alignment) {
    return alignDown<Power2Alignment>(addr + alignment - 1, alignment);
}

template <typename S, typename T>
inline S* offsetPointerByBytesAndAlign(T* ptr, ptrdiff_t bytes){
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    addr += static_cast<uintptr_t>(bytes);
    addr = alignUp<true>(addr, alignof(S));
    return reinterpret_cast<S*>(addr);
}

#endif //POINTERARITHMETIC_H
