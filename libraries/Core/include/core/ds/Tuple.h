//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_TUPLE_H
#define CROCOS_TUPLE_H

#include <stddef.h>
#include "../utility.h"

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
    [[nodiscard]] auto& get() {
        if constexpr (N == 0) {
            return (value);
        } else {
            return Tuple<Rest...>::template get<N - 1>();
        }
    }

    template<size_t N>
    [[nodiscard]] decltype(auto) get() const {
        if constexpr (N == 0) {
            return (value);
        } else {
            return Tuple<Rest...>::template get<N - 1>();
        }
    }

    auto& first() {
        return value;
    }

    auto& second() requires(sizeof...(Rest) >= 1) {
        return get<1>();
    }

    auto& third() requires(sizeof...(Rest) >= 2) {
        return get<2>();
    }

    const auto& first() const{
        return value;
    }

    const auto& second() const requires(sizeof...(Rest) >= 1){
        return get<1>();
    }

    const auto& third() const requires(sizeof...(Rest) >= 2){
        return get<2>();
    }
};

template<typename... Ts>
Tuple<Ts...> makeTuple(Ts&&... args) {
    return Tuple<Ts...>(forward<Ts>(args)...);
}

template<typename Stream, typename Tuple, size_t... Is>
void print_tuple(Stream& ps, const Tuple& t, index_sequence<Is...>) {
    ((ps << (Is == 0 ? "(" : ", ") << t.template get<Is>()), ...);
    ps << ")";
}

namespace Core {
    class PrintStream;
}

template<typename... Ts>
Core::PrintStream& operator <<(Core::PrintStream& ps, const Tuple<Ts...>& var) requires(all_streamable_v<Core::PrintStream, Ts...>){
    print_tuple(ps, var, make_index_sequence<sizeof...(Ts)>());
    return ps;
}
#endif //CROCOS_TUPLE_H
