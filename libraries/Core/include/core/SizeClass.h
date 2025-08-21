//
// Created by Spencer Martin on 8/20/25.
//

#ifndef SIZECLASS_H
#define SIZECLASS_H

#include <core/math.h>
#include <core/utility.h>

template <typename T, size_t N, ConstexprArray<T, N> array>
requires (isArraySorted(array) && N > 0)
constexpr ConstexprArray<T, log2floor(array[N-1])> _makeSizeClassJumpTableImpl(){
    constexpr size_t M = log2floor(array[N-1]);
    ConstexprArray<T, M> jumpTable;
    size_t index = 0; //the index into array that we are comparing against
    for(size_t i = 0; i < M; i++){
        while(array[index] < (1 << i)) index++;
        jumpTable[i] = index;
    }

    return jumpTable;
};

template <auto array>
requires IsConstexprArray<decltype(array)>
constexpr auto makeSizeClassJumpTable() {
    using T = decltype(array);
    return _makeSizeClassJumpTableImpl<typename T::Type, T::size(), array>();
}

template <auto array>
requires IsConstexprArray<decltype(array)>
//If I feel really silly, I might go back and add an optional binary search of the array contains many long runs of
//elements between consecutive powers of 2 at some point. But I do not anticipate this situation coming up in practice
size_t sizeClassIndex(typename decltype(array)::Type size) {
    static constexpr auto jumpTable = makeSizeClassJumpTable<array>();
    if (size == 0) return 0;
    constexpr size_t npos = static_cast<size_t>(-1);
    size_t log2Size = log2floor(size);
    if (log2Size >= jumpTable.size()) return npos;
    for (size_t index = jumpTable[log2Size]; index < jumpTable.size(); ++index) {
        if (size <= array[index]) return index;
    }
    return npos;
}

#endif //SIZECLASS_H
