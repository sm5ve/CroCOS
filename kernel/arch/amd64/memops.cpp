//
// Created by Spencer Martin on 2/18/25.
//

#include <stddef.h>
#include <stdint.h>

extern "C" void* memset(void* dest, int value, size_t len) {
    uint8_t* d = (uint8_t*)dest;
    for(size_t i = 0; i < len; i++){
        d[i] = (uint8_t)value;
    }
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dest;
    uint8_t* s = (uint8_t*)src;
    for(size_t i = 0; i < len; i++){
        d[i] = s[i];
    }
    return dest;
}