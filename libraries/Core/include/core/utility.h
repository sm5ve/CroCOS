//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_UTILITY_H
#define CROCOS_UTILITY_H

#include "TypeTraits.h"

template <typename T, T v>
struct integral_constant {
    static constexpr T value = v;
    using value_type = T;
    using type = integral_constant;  // The type of the trait
    constexpr operator T() const noexcept { return value; }
};

using true_type = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

template <typename T>
struct remove_reference {
    using type = T;  // Default case: T is not a reference, so the type is just T
};

template <typename T>
struct remove_reference<T&> {
    using type = T;  // If T is a reference, remove the reference part
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;  // If T is an rvalue reference, remove the reference part
};

// Helper alias template to make usage easier
template <typename T>
using remove_reference_t = typename remove_reference<T>::type;

template <typename T>
struct is_lvalue_reference : false_type {};  // Default case: T is not an lvalue reference

template <typename T>
struct is_lvalue_reference<T&> : true_type {};  // Specialization: T is an lvalue reference

template <typename T>
constexpr T&& move(T& t) noexcept {
    return static_cast<T&&>(t);
}

template <typename T>
T&& forward(remove_reference_t<T>& arg) noexcept {
    return static_cast<T&&>(arg);
}

template <typename T>
T&& forward(remove_reference_t<T>&& arg) noexcept {
    static_assert(!is_lvalue_reference<T>::value, "bad forward");
    return static_cast<T&&>(arg);
}

template<typename T> struct is_integral { static constexpr bool value = false; };

template<> struct is_integral<bool>              { static constexpr bool value = true; };
template<> struct is_integral<char>              { static constexpr bool value = true; };
template<> struct is_integral<signed char>       { static constexpr bool value = true; };
template<> struct is_integral<unsigned char>     { static constexpr bool value = true; };
template<> struct is_integral<short>             { static constexpr bool value = true; };
template<> struct is_integral<unsigned short>    { static constexpr bool value = true; };
template<> struct is_integral<int>               { static constexpr bool value = true; };
template<> struct is_integral<unsigned int>      { static constexpr bool value = true; };
template<> struct is_integral<long>              { static constexpr bool value = true; };
template<> struct is_integral<unsigned long>     { static constexpr bool value = true; };
template<> struct is_integral<long long>         { static constexpr bool value = true; };
template<> struct is_integral<unsigned long long>{ static constexpr bool value = true; };

template<typename T>
constexpr bool is_integral_v = is_integral<T>::value;

template <bool C, typename A, typename B>
struct conditional;

template <typename A, typename B>
struct conditional<true, A, B>{
    using type = A;
};

template <typename A, typename B>
struct conditional<false, A, B>{
    using type = B;
};

template <bool C, typename A, typename B>
using conditional_t = conditional<C, A, B>::type;

template<typename T, typename S>
struct is_same {
    static constexpr bool value = false;
};

template<typename T>
struct is_same<T, T> {
    static constexpr bool value = true;
};

template<typename T, typename S>
constexpr bool is_same_v = is_same<T, S>::value;

template <typename T, typename... Ts>
struct IndexOfHelper {
    static constexpr size_t value = static_cast<size_t>(-1);
};

template <typename T, typename First, typename... Rest>
struct IndexOfHelper<T, First, Rest...> {
    static constexpr size_t value = is_same_v<T, First>
                                    ? 0
                                    : (IndexOfHelper<T, Rest...>::value == static_cast<size_t>(-1)
                                       ? static_cast<size_t>(-1)
                                       : 1 + IndexOfHelper<T, Rest...>::value);
};

template <typename T, typename... Ts>
using IndexOf = IndexOfHelper<T, Ts...>;

template <size_t Index, typename... Ts>
struct TypeAt;

template <size_t Index, typename First, typename... Rest>
struct TypeAt<Index, First, Rest...> {
    using type = typename TypeAt<Index - 1, Rest...>::type;
};

template <typename First, typename... Rest>
struct TypeAt<0, First, Rest...> {
    using type = First;
};

template<bool B, typename T = void>
struct enable_if {};

template<typename T>
struct enable_if<true, T> {
    using type = T;
};

template<bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<typename F, typename Arg>
constexpr auto invoke(F&& f, Arg&& arg) -> decltype(f(arg)) {
    return f(arg);
}

template<size_t... Is>
struct index_sequence { };

template<size_t N, size_t... Is>
struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, Is...> { };

template<size_t... Is>
struct make_index_sequence_impl<0, Is...> {
    using type = index_sequence<Is...>;
};

template <typename T>
T&& declval() noexcept;

template<typename...> using void_t = void;

// Primary template — not defined (intentionally incomplete)
template<typename, typename F, typename... Args>
struct invoke_result_impl;

// Specialization — only defined when F(Args...) is well-formed
template<typename F, typename... Args>
struct invoke_result_impl<void_t<decltype(declval<F>()(declval<Args>()...))>, F, Args...> {
    using type = decltype(declval<F>()(declval<Args>()...));
};

template<typename F, typename... Args>
using invoke_result = invoke_result_impl<void, F, Args...>;

template<typename F, typename... Args>
using invoke_result_t = typename invoke_result<F, Args...>::type;

template<size_t N>
using make_index_sequence = typename make_index_sequence_impl<N>::type;

// remove_cv
template<typename T> struct remove_cv                { using type = T; };
template<typename T> struct remove_cv<const T>       { using type = T; };
template<typename T> struct remove_cv<volatile T>    { using type = T; };
template<typename T> struct remove_cv<const volatile T> { using type = T; };

// decay
template<typename T>
struct decay {
    using U = typename remove_reference<T>::type;
    using type = typename remove_cv<U>::type;
};

template<typename... Ts>
struct type_list {};

template<typename A, typename B>
struct concat;

template<typename... As, typename... Bs>
struct concat<type_list<As...>, type_list<Bs...>> {
    using type = type_list<As..., Bs...>;
};

template<typename List>
struct unique_type_list;

template<>
struct unique_type_list<type_list<>> {
    using type = type_list<>;
};

template<typename T, typename List>
struct prepend;

template<typename T, typename... Ts>
struct prepend<T, type_list<Ts...>> {
    using type = type_list<T, Ts...>;
};

template<typename T, typename... Rest>
struct unique_type_list<type_list<T, Rest...>> {
private:
using tail = typename unique_type_list<type_list<Rest...>>::type;
static constexpr bool exists = (is_same_v<T, Rest> || ...);

public:
using type = conditional_t<
        exists,
        tail,
        typename prepend<T, tail>::type
>;
};

template<typename T>
using decay_t = typename decay<T>::type;

template <typename... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};

template<typename F, typename Arg, typename = void>
struct is_invocable : false_type {};

template<typename F, typename Arg>
struct is_invocable<F, Arg, void_t<decltype(void(declval<F&>()(declval<Arg>())) )>>
        : true_type {};

template<typename TypeList, typename F>
struct transform_result_types;

template<typename... Ts, typename F>
struct transform_result_types<type_list<Ts...>, F> {
    using type = type_list<invoke_result_t<F, Ts>...>;
};

template <typename... Ts>
overload(Ts...) -> overload<Ts...>;

// Non-allocating placement new
inline void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

// Matching placement delete (not required, but good practice)
inline void operator delete(void*, void*) noexcept {}

template <typename T>
inline void swap(T& t1, T& t2){
    T temp = move(t1);
    t1 = move(t2);
    t2 = move(temp);
}

template <typename T>
//Sets t2 to t1, t3 to t2, and t1 to t3
inline void rotateRight(T& t1, T& t2, T& t3){
    T temp = move(t3);
    t3 = move(t2);
    t2 = move(t1);
    t1 = move(temp);
}

template <typename T>
//Sets t1 to t2, t2 to t3, and t3 to t1
inline void rotateLeft(T& t1, T& t2, T& t3){
    T temp = move(t1);
    t1 = move(t2);
    t2 = move(t3);
    t3 = move(temp);
}

template <typename T>
inline T min(T t1, T t2){
    return t1 < t2 ? t1 : t2;
}

template <typename T>
inline T max(T t1, T t2){
    return t1 > t2 ? t1 : t2;
}

template <typename>
class FunctionRef;

template<typename Ret, typename... Args>
class FunctionRef<Ret(Args...)> {
    using CallbackFn = Ret (*)(void*, Args...);

    void* obj = nullptr;
    CallbackFn callback = nullptr;

public:
    FunctionRef() = default;

    template<typename Callable>
    FunctionRef(Callable& f) {
        obj = static_cast<void*>(&f);
        callback = [](void* o, Args... args) -> Ret {
            return (*static_cast<Callable*>(o))(args...);
        };
    }

    Ret operator()(Args... args) const {
        return callback(obj, args...);
    }

    explicit operator bool() const {
        return callback != nullptr;
    }

    bool operator==(FunctionRef<Ret(Args ...)> other) const {
        return (other.callback == callback) && (other.obj == obj);
    }
};

template<typename From, typename To>
concept convertible_to = requires(From f) {
    static_cast<To>(f);
};

template<typename T>
concept comparable_less_than = requires(T a, T b) {
    { a < b } -> convertible_to<bool>;
};

template<typename T>
concept comparable_equality = requires(T a, T b) {
    { a == b } -> convertible_to<bool>;
};

template<typename... Ts>
struct TypeSetComparator{
private:
    template<typename R>
    static constexpr bool exists(){
        return (is_same_v<R, Ts> || ...);
    }
public:
    template<typename... Rs>
    static constexpr bool contains(){
        return (exists<Rs>() && ...);
    }
};

template<typename Stream, typename T, typename = void>
struct is_streamable : false_type {};

template<typename Stream, typename T>
struct is_streamable<Stream, T,
        decltype(void(declval<Stream&>() << declval<T>()))> : true_type {};

template<typename Stream, typename... Ts>
constexpr bool all_streamable_v = (is_streamable<Stream, Ts>::value && ...);

struct monostate {
    constexpr monostate() noexcept = default;

    friend bool operator==(const monostate&, const monostate&) noexcept {
        return true;
    }

    friend bool operator!=(const monostate&, const monostate&) noexcept {
        return false;
    }
};

template<typename T>
struct TypeID {
    constexpr static uint64_t value() {
        static const uint64_t id = reinterpret_cast<uint64_t>(&id);
        return id;
    }
};

template<typename T>
inline constexpr uint64_t TypeID_v = TypeID<T>::value();

template<typename Base, typename Derived>
struct is_base_of {
private:
    // Try to convert Derived* to Base*.
    static char test(Base*);
    static long test(...);

public:
    static constexpr bool value = sizeof(test(static_cast<Derived*>(nullptr))) == sizeof(char);
};

template<typename Base, typename Derived>
inline constexpr bool is_base_of_v = is_base_of<Base, Derived>::value;

template<typename Base, typename Derived>
concept IsBaseOf = is_base_of_v<Base, Derived>;

template<typename From, typename To>
concept StaticCastable = requires(From from) {
    static_cast<To>(from);
};

template<typename From, typename To>
constexpr bool is_static_castable_v = StaticCastable<From, To>;

// Primary template - not a pointer
template<typename T>
struct is_pointer : false_type {};

// Specializations for pointer types
template<typename T>
struct is_pointer<T*> : true_type {};

template<typename T>
struct is_pointer<T* const> : true_type {};

template<typename T>
struct is_pointer<T* volatile> : true_type {};

template<typename T>
struct is_pointer<T* const volatile> : true_type {};

// Variable template for convenience
template<typename T>
constexpr bool is_pointer_v = is_pointer<T>::value;

template<typename T>
concept IsPointer = is_pointer_v<T>;

// Primary template - not a reference
template<typename T>
struct is_reference : false_type {};

// Specializations for reference types
template<typename T>
struct is_reference<T&> : true_type {};

template<typename T>
struct is_reference<T const&> : true_type {};

template<typename T>
struct is_reference<T volatile&> : true_type {};

template<typename T>
struct is_reference<T const volatile&> : true_type {};

// Variable template for convenience
template<typename T>
constexpr bool is_reference_v = is_reference<T>::value;

template<typename T>
concept IsReference = is_reference_v<T>;

template<typename T>
struct remove_pointer;

// Specializations for pointer types
template<typename T>
struct remove_pointer<T*> {
    using type = T;
};

template<typename T>
struct remove_pointer<T* const> {
    using type = T;
};

template<typename T>
struct remove_pointer<T* volatile> {
    using type = T;
};

template<typename T>
struct remove_pointer<T* const volatile> {
    using type = T;
};

#endif //CROCOS_UTILITY_H
