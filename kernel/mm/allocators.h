//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_ALLOCATORS_H
#define CROCOS_ALLOCATORS_H

#include "stddef.h"

namespace kernel::mm::allocators{
    void* bump_alloc(size_t size, std::align_val_t align = std::align_val_t{1});
    bool in_bump_alloc_range(void* );
}

#endif //CROCOS_ALLOCATORS_H
