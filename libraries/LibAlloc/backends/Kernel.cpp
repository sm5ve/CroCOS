//
// Created by Spencer Martin on 8/1/25.
//
#include <kassert.h>
#include <liballoc/Backend.h>

namespace LibAlloc::Backend{
    const size_t smallPageSize = 4096;
    const size_t largePageSize = smallPageSize * 512;

    void* allocPages(size_t count) {
        (void)count;
        assertUnimplemented("We have not yet hooked up LibAlloc to the memory manager");
        return nullptr;
    }
    //The caller is responsible for retaining information on page counts of allocations
    void freePages(void* ptr, size_t count) {
        (void)ptr; (void)count;
        assertUnimplemented("We have not yet hooked up LibAlloc to the memory manager");
    }
}