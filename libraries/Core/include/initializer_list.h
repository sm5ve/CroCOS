//
// Created by Spencer Martin on 8/3/25.
//

#ifndef INITIALIZER_LIST_H
#define INITIALIZER_LIST_H

#include <stddef.h>

#ifndef __clang__

namespace std{
    template<typename T>
    class initializer_list{
        const T* _begin;
        size_t length;

        inline constexpr initializer_list(const T* begin, size_t len) : _begin(begin), length(len){}
    public:
        inline constexpr initializer_list() : _begin(nullptr), length(0){}

        inline constexpr size_t size() const { return length; }

        inline constexpr T const* begin() const { return _begin; }

        inline constexpr T const* end() const { return _begin + length; }
    };
}

#endif


#endif //INITIALIZER_LIST_H
