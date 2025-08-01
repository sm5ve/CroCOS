//
// Created by Spencer Martin on 8/1/25.
//

#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>

namespace LibAlloc::Backend{
    extern const size_t smallPageSize;
    extern const size_t largePageSize;

    void* allocPages(size_t count);
    //The caller is responsible for retaining information on page counts of allocations
    void freePages(void* ptr, size_t count);
    //In the future we may extend this with madvise type calls where necessary.
}

#endif //BACKEND_H
