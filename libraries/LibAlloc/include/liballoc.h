//
// Created by Spencer Martin on 8/23/25.
//

#ifndef LIBALLOC_H
#define LIBALLOC_H

#include <stddef.h>

void la_init();
void la_init(void* buffer, size_t size);

void* la_malloc(size_t size, std::align_val_t align = std::align_val_t(alignof(size_t)));
void la_free(void* p);

#endif //LIBALLOC_H