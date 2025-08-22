//
// Created by Spencer Martin on 8/21/25.
//

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

namespace LibAlloc{
    class Allocator{
    public:
        virtual ~Allocator() = default;

        virtual void* allocate(size_t size, std::align_val_t align) = 0;
        virtual bool free(void* ptr) = 0;
    };
}

#endif //ALLOCATOR_H
