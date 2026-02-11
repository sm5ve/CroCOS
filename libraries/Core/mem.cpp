//
// Created by Spencer Martin on 7/25/25.
//
#include <core/mem.h>
#include <stdint.h>

//I think gcc's builtin implementations of memcpy and memset use SSE, and so won't work in the kernel right now.
#ifndef KERNEL
#define USE_BUILTINS
#endif


extern "C" void* memset(void* dest, int value, size_t len) {
#if __has_builtin(__builtin_memset) && defined(USE_BUILTINS)
    __builtin_memset(dest, value, len);
    return dest;
#else
    uint8_t* d = (uint8_t*)dest;
    for(size_t i = 0; i < len; i++){
        d[i] = (uint8_t)value;
    }
    return dest;
#endif
}

extern "C" void* memcpy(void* dest, const void* src, size_t len) {
#if __has_builtin(__builtin_memcpy) && defined(USE_BUILTINS)
    __builtin_memcpy(dest, src, len);
    return dest;
#else
    uint8_t* d = (uint8_t*)dest;
    uint8_t* s = (uint8_t*)src;
    for(size_t i = 0; i < len; i++){
        d[i] = s[i];
    }
    return dest;
#endif
}