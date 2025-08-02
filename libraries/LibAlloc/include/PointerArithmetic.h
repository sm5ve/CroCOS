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

#endif //POINTERARITHMETIC_H
