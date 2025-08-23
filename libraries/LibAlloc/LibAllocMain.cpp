//
// Created by Spencer Martin on 8/23/25.
//
#include <liballoc.h>
#include <liballoc/InternalAllocator.h>

void la_init(void* buffer, size_t size) {
    LibAlloc::InternalAllocator::initializeInternalAllocator();
    if(buffer != nullptr) {
        LibAlloc::InternalAllocator::grantBuffer(buffer, size);
    }
}

void la_init(){
    la_init(nullptr, 0);
}

void* la_malloc(size_t size, std::align_val_t align) {
    return LibAlloc::InternalAllocator::malloc(size, align);
}

void la_free(void* ptr) {
    LibAlloc::InternalAllocator::free(ptr);
}