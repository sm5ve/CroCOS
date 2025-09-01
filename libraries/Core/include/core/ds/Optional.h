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
concept Nullable = requires(T& t, T& s)
{
    {t == s} -> convertible_to<bool>;
    {T::null()} -> convertible_to<T>;
};

template <typename T>
requires Nullable<T>
class ImplicitOptional;

template <typename T>
class ExplicitOptional;

template <typename T>
struct _Optional {
    using Type = ExplicitOptional<T>;
};

template <typename T>
requires Nullable<T>
struct _Optional <T>{
    using Type = ImplicitOptional<T>;
};

template <typename T>
using Optional = _Optional<T>::Type;

template <typename T>
struct is_optional : false_type {};

template <typename U>
struct is_optional<ImplicitOptional<U>> : true_type {};

template <typename U>
struct is_optional<ExplicitOptional<U>> : true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template<typename T>
requires Nullable<T>
class ImplicitOptional {
    T value;
public:
    constexpr static T null() {
        return T::null();
    }

    ImplicitOptional(){
        value = T::null();
    }

    ImplicitOptional(T& t) : value(t){
    }

    ImplicitOptional(const T& t) : value(t){
    }

    ImplicitOptional(T&& t) : value(move(t)){
    }

    bool occupied() const{
        return value != T::null();
    }

    T* operator->() {
        return &value;
    }

    T* as_ptr() {
        return &value;
    }

    const T* operator->() const {
        return &value;
    }

    const T* as_ptr() const {
        return &value;
    }

    bool operator==(const ImplicitOptional & other) const {
        return value == other.value;
    }

    T& operator*(){
        assert(occupied(), "Tried to dereference empty optional");
        return value;
    }

    const T& operator*() const{
        assert(occupied(), "Tried to dereference empty optional");
        return value;
    }

    explicit operator bool() const { return occupied(); }

    T& value_or(T& t) {
        if (occupied()) return value;
        return t;
    }

    T& value_or(T& t) const{
        if (occupied()) return value;
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

template<typename T>
class ExplicitOptional{
    static_assert(!is_same_v<T, monostate>, "Can't make an optional monostate!");
private:
    Variant<T, monostate> value;

public:
    constexpr static ExplicitOptional null() {
        return {};
    }

    ExplicitOptional() {
        new(&value) Variant<T, monostate>(monostate{});
    }

    ExplicitOptional(T& t){
        new(&value) Variant<T, monostate>(t);
    }

    ExplicitOptional(const T& t){
        new(&value) Variant<T, monostate>(t);
    }

    ExplicitOptional(T&& t){
        new(&value) Variant<T, monostate>(move(t));
    }

    template<typename ... Ts>
    void emplace(Ts... ts){
        value.template emplace<T>(ts...);
    }

    bool operator==(const ExplicitOptional & other) const {
        return value == other.value;
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

    T& value_or(T& t) const{
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
            return ExplicitOptional<OutType>(outVal);
        }
        return ExplicitOptional<OutType>();
    }

    template<typename F>
    auto transform(F&& fn) &&{
        using OutType = invoke_result_t<F, T&>;
        if(occupied()){
            auto outVal = invoke(fn, **this);
            return ExplicitOptional<OutType>(outVal);
        }
        return ExplicitOptional<OutType>();
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
