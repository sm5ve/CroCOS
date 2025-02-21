//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_KERNEL_H
#define CROCOS_KERNEL_H

#include <lib/PrintStream.h>
#include "stddef.h"
#include <utility.h>

namespace kernel{
    extern PrintStream& DbgOut;
    void* kmalloc(size_t size, std::align_val_t = std::align_val_t{1});
    void kfree(void* ptr);
}

extern "C" void* memset(void* dest, int value, size_t len);
extern "C" void* memswap(void* dest, const void* src, size_t len);
extern "C" void* memcpy(void* dest, const void* src, size_t len);

#endif //CROCOS_KERNEL_H
