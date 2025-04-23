//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_TUPLE_H
#define CROCOS_TUPLE_H

#include <stddef.h>
#include "utility.h"

template<typename... Ts>
struct Tuple;

template<>
struct Tuple<> { };

// Recursive case: Tuple<T, Rest...> stores one value and inherits from the rest
template<typename T, typename... Rest>
struct Tuple<T, Rest...> : Tuple<Rest...> {
    T value;

    Tuple() = default;

    Tuple(T t, Rest... rest)
            : Tuple<Rest...>(rest...), value(t) {}

    // Get element by index
    template<size_t N>
    decltype(auto) get() {
        if constexpr (N == 0) {
            return (value);
        } else {
            return Tuple<Rest...>::template get<N - 1>();
        }
    }

    template<size_t N>
    decltype(auto) get() const {
        if constexpr (N == 0) {
            return (value);
        } else {
            return Tuple<Rest...>::template get<N - 1>();
        }
    }
};

template<typename... Ts>
Tuple<Ts...> makeTuple(Ts&&... args) {
    return Tuple<Ts...>(forward<Ts>(args)...);
}
#endif //CROCOS_TUPLE_H
