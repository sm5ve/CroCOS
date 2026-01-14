//
// Created by Spencer Martin on 2/15/25.
//
#include <kernel.h>
#include <liballoc.h>
#include <kconfig.h>

//bool heap_initialized = false;

uint8_t heap_buffer[KERNEL_INIT_HEAP_BUFFER];

namespace kernel{
    bool heapEarlyInit() {
        la_init(heap_buffer, sizeof(heap_buffer));
        return true;
    }

    void* kmalloc(size_t size, std::align_val_t align){
        return la_malloc(size, align);
    }

    void kfree(void* ptr){
        la_free(ptr);
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

