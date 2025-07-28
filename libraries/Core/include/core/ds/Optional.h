//
// Created by Spencer Martin on 4/22/25.
//

#ifndef CROCOS_OPTIONAL_H
#define CROCOS_OPTIONAL_H

#include "../utility.h"
#include "Variant.h"
#include "../TypeTraits.h"
#include <assert.h>

template <typename T>
class Optional;

template <typename T>
struct is_optional : false_type {};

template <typename U>
struct is_optional<Optional<U>> : true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template<typename T>
class Optional{
    static_assert(!is_same_v<T, monostate>, "Can't make an optional monostate!");
private:
    Variant<T, monostate> value;

public:
    Optional(){
        value.template emplace<monostate>();
    }

    Optional(T& t){
        value.template emplace<T>(t);
    }

    Optional(const T& t){
        value.template emplace<T>(t);
    }

    Optional(T&& t){
        value.template emplace<T>(forward<T>(t));
    }

    template<typename ... Ts>
    void emplace(Ts... ts){
        value.template emplace<T>(ts...);
    }

    T* operator->() {
        return value.template get_if<T>();
    }

    bool occupied() const{
        return value.template holds<T>();
    }

    const T* operator->() const {
        return value.template get_if<T>();
    }

    T* as_ptr(){
        return value.template get_if<T>();
    }

    T& operator*(){
        assert(occupied(), "Tried to dereference empty optional");
        return value.template get<T>();
    }

    const T& operator*() const{
        assert(occupied(), "Tried to dereference empty optional");
        return value.template get<T>();
    }

    explicit operator bool() const { return occupied(); }

    T& value_or(T& t){
        if(occupied()){
            return *this;
        }
        return t;
    }

    template<typename F>
    auto transform(F&& fn) const &{
        using OutType = invoke_result_t<F, const T&>;
        if(occupied()) {
            auto outVal = invoke(fn, **this);
            return Optional<OutType>(outVal);
        }
        return Optional<OutType>();
    }

    template<typename F>
    auto transform(F&& fn) &&{
        using OutType = invoke_result_t<F, T&>;
        if(occupied()){
            auto outVal = invoke(fn, **this);
            return Optional<OutType>(outVal);
        }
        return Optional<OutType>();
    }

    template<typename F>
    auto and_then(F&& fn) const & -> enable_if_t<
            is_optional_v<invoke_result_t<F, const T&>>,invoke_result_t<F, const T&>>{
        using OutType = invoke_result_t<F, const T&>;
        if(occupied())
            return invoke(fn, **this);
        return OutType();
    }

    template<typename F>
    auto and_then(F&& fn) && -> enable_if_t<
            is_optional_v<invoke_result_t<F, const T&>>, invoke_result_t<F, const T&>>{
        using OutType = invoke_result_t<F, T>;
        if(occupied())
            return invoke(fn, **this);
        return OutType();
    }

    template<typename F>
    auto operator|(F&& fn) && {
        using result_t = invoke_result_t<F, T>;
        constexpr bool is_optional_result = is_optional_v<decay_t<result_t>>;
        if constexpr (is_void_v<result_t>){
            if(occupied()){
                fn(**this);
            }
            return;
        }
        if constexpr(is_optional_result){
            return this -> and_then(fn);
        }
        else{
            return this -> transform(fn);
        }
    }

    template<typename F>
    auto operator|(F&& fn) const & {
        using result_t = invoke_result_t<F, T>;
        constexpr bool is_optional_result = is_optional_v<decay_t<result_t>>;
        if constexpr (is_void_v<result_t>){
            if(occupied()){
                fn(**this);
            }
            return;
        }
        if constexpr(is_optional_result){
            return this -> and_then(fn);
        }
        else{
            return this -> transform(fn);
        }
    }


};

namespace Core{
    class PrintStream;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
    PrintStream& operator <<(PrintStream& ps, char* message);

#pragma GCC diagnostic pop
}

template<typename T>
Core::PrintStream& operator <<(Core::PrintStream& ps, Optional<T>& var) requires(is_streamable<Core::PrintStream, T>::value){
    ps << "Optional<" << type_name<T>() << ">(";
    if(var.occupied()){
        ps << *var;
    }
    return ps << ")";
}

template<typename T>
Core::PrintStream& operator <<(Core::PrintStream& ps, const Optional<T>& var) requires(is_streamable<Core::PrintStream, T>::value){
    ps << "Optional<" << type_name<T>() << ">(";
    if(var.occupied()){
        ps << *var;
    }
    return ps << ")";
}

#endif //CROCOS_OPTIONAL_H
