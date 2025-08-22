//
// Created by Spencer Martin on 8/1/25.
//

#ifndef POINTERARITHMETIC_H
#define POINTERARITHMETIC_H

#include <stddef.h>
#include <stdint.h>

template <typename S, typename T>
inline S* offsetPointerByBytes(T* ptr, int64_t bytes){
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    return reinterpret_cast<S*>(addr + bytes);
}

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

#endif //POINTERARITHMETIC_H
