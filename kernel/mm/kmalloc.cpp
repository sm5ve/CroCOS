//
// Created by Spencer Martin on 2/15/25.
//
#include <kernel.h>
#include "allocators.h"

bool heap_initialized = false;

namespace kernel{
    void* kmalloc(size_t size, std::align_val_t align){
        if(!heap_initialized){
            return kernel::mm::allocators::bump_alloc(size, align);
        }
        return nullptr;
    }

    void kfree(void* ptr){
        if(ptr == nullptr){
            return;
        }
    }
}

void *operator new(size_t size)
{
    return kernel::kmalloc(size);
}

void *operator new[](size_t size)
{
    return kernel::kmalloc(size);
}

void *operator new(size_t size, std::align_val_t align)
{
    return kernel::kmalloc(size, align);
}

void *operator new[](size_t size, std::align_val_t align)
{
    return kernel::kmalloc(size, align);
}

void operator delete(void *p)
{
    kernel::kfree(p);
}

void operator delete[](void *p)
{
    kernel::kfree(p);
}

