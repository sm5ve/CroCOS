//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_UTILITY_H
#define CROCOS_UTILITY_H
namespace std{
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
    struct is_lvalue_reference : std::false_type {};  // Default case: T is not an lvalue reference

    template <typename T>
    struct is_lvalue_reference<T&> : std::true_type {};  // Specialization: T is an lvalue reference


    template <typename T>
    constexpr T&& move(T& t) noexcept {
        return static_cast<T&&>(t);
    }

    template <typename T>
    T&& forward(std::remove_reference_t<T>& arg) noexcept {
        return static_cast<T&&>(arg);
    }

    template <typename T>
    T&& forward(std::remove_reference_t<T>&& arg) noexcept {
        static_assert(!std::is_lvalue_reference<T>::value, "bad forward");
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
}

// Non-allocating placement new
inline void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

// Matching placement delete (not required, but good practice)
inline void operator delete(void*, void*) noexcept {}

template <typename T>
inline void swap(T& t1, T& t2){
    T temp = std::move(t1);
    t1 = std::move(t2);
    t2 = std::move(temp);
}

template <typename T>
//Sets t2 to t1, t3 to t2, and t1 to t3
inline void rotateRight(T& t1, T& t2, T& t3){
    T temp = std::move(t3);
    t3 = std::move(t2);
    t2 = std::move(t1);
    t1 = std::move(temp);
}

template <typename T>
//Sets t1 to t2, t2 to t3, and t3 to t1
inline void rotateLeft(T& t1, T& t2, T& t3){
    T temp = std::move(t1);
    t1 = std::move(t2);
    t2 = std::move(t3);
    t3 = std::move(temp);
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
#endif //CROCOS_UTILITY_H
