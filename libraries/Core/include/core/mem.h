//
// Created by Spencer Martin on 7/25/25.
//

#ifndef MEM_H
#define MEM_H
#include <stddef.h>

extern "C" void* memset(void* dest, int value, size_t len);
extern "C" void* memswap(void* dest, const void* src, size_t len);
extern "C" void* memcpy(void* dest, const void* src, size_t len);
#endif //MEM_H
