//
// Created by Spencer Martin on 3/31/26.
//
// Non-owning view over a contiguous array of const elements.
//

#ifndef CROCOS_SPAN_H
#define CROCOS_SPAN_H

#include <stddef.h>
#include <assert.h>

template<typename T>
struct Span {
    const T* ptr   = nullptr;
    size_t   count = 0;

    const T* begin() const { return ptr; }
    const T* end()   const { return ptr + count; }
    size_t   size()  const { return count; }
    const T& operator[](size_t i) const { assert(i < count, "Span index out of bounds"); return ptr[i]; }
};

#endif // CROCOS_SPAN_H
