//
// Created by Spencer Martin on 4/21/25.
//

#ifndef CROCOS_VARIANT_H
#define CROCOS_VARIANT_H
#include "../utility.h"
#include "../math.h"
#include <assert.h>
#include <stddef.h>

template <typename... Ts>
constexpr size_t LCMAlign = ([] {
    constexpr size_t aligns[] = {alignof(Ts)...};
    size_t result = aligns[0];
    for (size_t i = 1; i < sizeof...(Ts); ++i) {
        result = lcm(result, aligns[i]);
    }
    return result;
}());

template <typename... Ts>
struct VariantStorage {
    static const size_t align = LCMAlign<Ts...>;
    alignas(align) unsigned char data[max(sizeof(Ts)...)];

    void* raw() { return static_cast<void*>(data); }
    const void* raw() const { return static_cast<const void*>(data); }
};

template <typename... Ts>
class Variant {
private:

    VariantStorage<Ts...> storage;
    size_t index = static_cast<size_t>(-1);

    using DestructorFunc = void(*)(void*);
    static constexpr DestructorFunc destructors[] = {
            [](void* ptr) { assert(ptr != nullptr, "AAAA"); reinterpret_cast<Ts*>(ptr)->~Ts(); }...
    };

    template<typename VisitorT, size_t... Is>
    auto visit_impl(VisitorT&& visitor, index_sequence<Is...>) {
        using ReturnT = typename AllSameReturn<VisitorT, Ts...>::type;

        if constexpr (is_void_v<ReturnT>) {
            ((void)(index == Is && ([&] {
                visitor(get<typename TypeAt<Is, Ts...>::type>());
                return true;
            }())) || ...);
        } else {
            ReturnT result{};
            ((void)(index == Is && ([&] {
                result = visitor(get<typename TypeAt<Is, Ts...>::type>());
                return true;
            }())) || ...);
            return result;
        }
    }

    template <typename Visitor, typename... Args>
    struct AllSameReturn;

    template <typename Visitor, typename First, typename... Rest>
    struct AllSameReturn<Visitor, First, Rest...> {
        using FirstT = invoke_result_t<Visitor, First>;
        static constexpr bool value = (is_same_v<FirstT, invoke_result_t<Visitor, Rest>> && ...);
        using type = FirstT;
    };

    template<typename TL>
    struct transformer{};

    template<typename... Types>
    struct transformer<type_list<Types...>>{
        template<typename T, size_t... Is>
        static Variant<Types...> apply_impl(Variant<Ts...>& var, index_sequence<Is...>, T fn) {
            using OutType = Variant<Types...>;
            alignas(VariantStorage<Types...>::align) uint8_t buff[sizeof(OutType)];
            OutType& result = *((OutType*)(void*)&buff);
            //Initialize the output to something invalid
            memset(&result, 0xff, sizeof(OutType));

            ((void)(var.which() == Is ?
              ([&](){
                  auto value = var.template get<typename TypeAt<Is, Ts...>::type>();
                  result = Variant<Types...>(invoke(fn, value));
              }(), true) : false), ...);

            return result;
        }

        template<typename T>
        static Variant<Types...> apply(Variant<Ts...>& var, T f) {
            return apply_impl(var, make_index_sequence<sizeof...(Ts)>{}, f);
        }
    };

    template<typename... Rs, size_t... Is>
    void move_impl(Variant<Rs...>&& var, index_sequence<Is...>){
        if(var.which() == (size_t)-1){
            index = (size_t)-1;
            return;
        }
        ((void)(var.which() == Is ?
          ([&]{
              using U = TypeAt<Is, Rs...>::type;
              index = IndexOf<U, Ts...>::value;
              new (storage.raw()) decay_t<U>(forward<U>(var.template get<U>()));
          })(), true : false), ...);
    }

    template<typename... Rs, size_t... Is>
    void copy_impl(const Variant<Rs...>& var, index_sequence<Is...>){
        if(var.which() == (size_t)-1){
            index = (size_t)-1;
            return;
        }
        ((void)(var.which() == Is ?
          ([&]{
              using U = TypeAt<Is, Rs...>::type;
              index = IndexOf<U, Ts...>::value;
              new (storage.raw()) decay_t<U>(var.template get<U>());
          })(), true : false), ...);
    }
public:
    Variant() {
        static_assert(IndexOf<monostate, Ts...>::value != -1, "Called default constructor without Variant containing monostate");
        new (&storage) monostate{};
        index = IndexOf<monostate, Ts...>::value;
    }

    template <typename T,
            enable_if_t<(IndexOf<decay_t<T>, Ts...>::value != size_t(-1)), bool> = true>
    Variant(T&& value) {
        using U = decay_t<T>;
        new (storage.raw()) U(forward<T>(value));
        index = IndexOf<U, Ts...>::value;
    }

    template <typename... Rs,
            enable_if_t<TypeSetComparator<Ts...>::template contains<Rs...>(), bool> = true>
    Variant(Variant<Rs...>&& value) {
        if((void*)&value == (void*)this) return;
        move_impl(move(value), make_index_sequence<sizeof...(Rs)>{});
        value.destroy();
    }

    template <typename... Rs,
            enable_if_t<TypeSetComparator<Ts...>::template contains<Rs...>(), bool> = true>
    Variant(Variant<Rs...>& value) {
        if((void*)&value == (void*)this) return;
        copy_impl(value, make_index_sequence<sizeof...(Rs)>{});
    }

    template <typename T,
            enable_if_t<(IndexOf<decay_t<T>, Ts...>::value != size_t(-1)), bool> = true>
    Variant(T& value) {
        using U = decay_t<T>;
        new (storage.raw()) U(value);
        index = IndexOf<U, Ts...>::value;
    }

    template <size_t N>
    Variant(const char (&arr)[N]) requires(IndexOf<const char*, Ts...>::value != size_t(-1)){
        new (storage.raw()) const char*{arr};
        index = IndexOf<const char*, Ts...>::value;
    }

    // Copy constructor
    Variant(const Variant& other) {
        if(this == &other) return;
        copy_impl(other, make_index_sequence<sizeof...(Ts)>{});
    }

    // Move constructor
    Variant(Variant&& other) noexcept {
        if(this == &other) return;
        move_impl(move(other), make_index_sequence<sizeof...(Ts)>{});
        other.destroy();
    }

    template <typename T, typename... Args>
    void emplace(Args&&... args) {
        destroy(); // destroy previous value
        new (storage.raw()) T(forward<Args>(args)...);
        index = IndexOf<T, Ts...>::value;
    }

    template <typename T>
    T& get() {
        auto typeIndex = IndexOf<T, Ts...>::value;
        assert(index == typeIndex, "Variant did not hold data of type ", type_name<T>());
        return *reinterpret_cast<T*>(storage.raw());
    }

    template <typename T>
    const T& get() const{
        auto typeIndex = IndexOf<T, Ts...>::value;
        assert(index == typeIndex, "Variant did not hold data of type ", type_name<T>());
        return *reinterpret_cast<const T*>(storage.raw());
    }

    template <typename T>
    bool holds() const{
        return (index != (size_t)-1) && (IndexOf<T, Ts...>::value == index);
    }

    template <typename T>
    T* get_if() {
        if(holds<T>()){
            return &get<T>();
        }
        return nullptr;
    }

    size_t which() const { return index; }

    void destroy() {
        if (index != size_t(-1)) {
            destructors[index](storage.raw());
            index = (size_t)-1;
        }
    }

    // Assignment operators
    template <typename T,
            enable_if_t<(IndexOf<decay_t<T>, Ts...>::value != size_t(-1)), bool> = true>
    Variant& operator=(T&& value) {
        using U = decay_t<T>;
        if (holds<U>()) {
            // If we already hold this type, use assignment instead of destroy+construct
            get<U>() = forward<T>(value);
        } else {
            destroy();
            new (storage.raw()) U(forward<T>(value));
            index = IndexOf<U, Ts...>::value;
        }
        return *this;
    }

    Variant& operator=(const Variant& other) {
        if (this == &other) return *this;
        destroy();
        copy_impl(other, make_index_sequence<sizeof...(Ts)>{});
        return *this;
    }

    Variant& operator=(Variant&& other) {
        if (this == &other) return *this;
        destroy();
        move_impl(move(other), make_index_sequence<sizeof...(Ts)>{});
        other.destroy();
        return *this;
    }

    template <typename... Rs,
            enable_if_t<TypeSetComparator<Ts...>::template contains<Rs...>(), bool> = true>
    Variant& operator=(const Variant<Rs...>& other) {
        if ((void*)this == (void*)&other) return *this;
        destroy();
        copy_impl(other, make_index_sequence<sizeof...(Rs)>{});
        return *this;
    }

    template <typename... Rs,
            enable_if_t<TypeSetComparator<Ts...>::template contains<Rs...>(), bool> = true>
    Variant& operator=(Variant<Rs...>&& other) {
        if ((void*)this == (void*)&other) return *this;
        destroy();
        move_impl(move(other), make_index_sequence<sizeof...(Rs)>{});
        other.destroy();
        return *this;
    }

    ~Variant() { destroy(); }

    template <typename... Fs>
    auto visit(Fs&&... fs) {
        auto visitor = overload{forward<Fs>(fs)...};

        static_assert(AllSameReturn<decltype(visitor), Ts...>::value,
                      "All lambdas must return the same type.");

        using ReturnT = typename AllSameReturn<decltype(visitor), Ts...>::type;
        return (ReturnT) visit_impl(visitor, make_index_sequence<sizeof...(Ts)>{});
    }

    template<typename... Fs>
    auto transform(Fs&&... fs) {
        auto transform = overload{forward<Fs>(fs)...};

        using transformed_types = typename transform_result_types<type_list<Ts...>, decltype(transform)>::type;
        using deduplicated_types = typename unique_type_list<transformed_types>::type;

        return transformer<deduplicated_types>::apply(*this, transform);
    }

    template<typename T>
    size_t indexForType(){
        return IndexOf<T, Ts...>::value;
    }
};

namespace Core{
    class PrintStream;
    PrintStream& operator <<(PrintStream& ps, char* message);
}

template<typename ... Ts, size_t ... Is>
void print_impl(Core::PrintStream& ps, Variant<Ts...>& var, index_sequence<Is...>){
    if(var.which() == (size_t)-1){
        ps << "Variant<>()";
    }
    else{
        ((void)(var.which() == Is ?
          ([&]{
              using U = TypeAt<Is, Ts...>::type;
              ps << "Variant<" << type_name<U>() << ">(" << var.template get<U>() << ")";
          })(), true : false), ...);
    }
}

template<typename ... Ts>
Core::PrintStream& operator <<(enable_if_t<all_streamable_v<Core::PrintStream, Ts...>, Core::PrintStream&> ps, Variant<Ts...>& var){
    print_impl(ps, var, make_index_sequence<sizeof...(Ts)>());
    return ps;
}
#endif //CROCOS_VARIANT_H
