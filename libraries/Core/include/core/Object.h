//
// Created by Spencer Martin on 5/25/25.
//

#ifndef OBJECT_H
#define OBJECT_H

#include <core/utility.h>
#include <core/TypeTraits.h>
#include <core/algo/sort.h>

class ObjectBase {
public:
    virtual ~ObjectBase() = default;
    virtual uint64_t type_id() = 0;
    virtual const char* type_name() = 0;
    virtual bool instanceof(uint64_t) const = 0;

    template <typename T>
    bool instanceof() const {
        return instanceof(TypeID_v<T>);
    }
};

// ============================
// Concept to detect metadata
// ============================

template<typename T>
concept HasObjectMetadata = requires {
    T::_crocos_flattened_types;
};


// Base case: empty list
template<typename... Ts>
struct FlattenedObjectBaseList;

// Partial specialization for empty pack
template<>
struct FlattenedObjectBaseList<> {
    using type = type_list<>;
};

template <typename T, bool Has>
struct flatten_select;

template <typename T>
struct flatten_select<T, true> {
    using type = decltype(T::_crocos_flattened_types);
};

template <typename T>
struct flatten_select<T, false> {
    using type = type_list<T>;
};

// Recursive step
template<typename T, typename... Ts>
struct FlattenedObjectBaseList<T, Ts...> {
private:
    using T_flat = typename flatten_select<T, HasObjectMetadata<T>>::type;

    using Tail_flat = typename FlattenedObjectBaseList<Ts...>::type;

public:
    using type = typename unique_type_list<typename concat<T_flat, Tail_flat>::type>::type;
};

template<typename... Ts>
constexpr uint64_t const (&make_type_id_array(type_list<Ts...>))[sizeof...(Ts)] {
    static constexpr uint64_t arr[sizeof...(Ts)] = { TypeID_v<Ts>... };
    return arr;
}

template<typename... Ts>
constexpr size_t get_type_array_size(type_list<Ts...>){
    return sizeof...(Ts);
}

// ============================
// Main Object Class
// ============================

template<size_t N>
class ObjectSortedParentIDs {
    public:
    uint64_t ids[N];
    ObjectSortedParentIDs(uint64_t* unsortedIDs) {
        memcpy(ids, unsortedIDs, N * sizeof(uint64_t));
        algorithm::sort(ids, N);
    }
};

template <typename T, typename... Bases>
class Object : public virtual ObjectBase, public Bases... {
private:
    template<typename S, typename... Bs>
    friend class Object;

public:
    using _CROCOS_FLATTENED_TYPES = typename unique_type_list<
        typename concat<type_list<T, ObjectBase>, typename FlattenedObjectBaseList<Bases...>::type>::type
    >::type;;
    static _CROCOS_FLATTENED_TYPES _crocos_flattened_types;

    static const size_t _crocos_obj_base_count = get_type_array_size(_crocos_flattened_types);
    static constexpr auto _crocos_const_flattened_ids = make_type_id_array(_crocos_flattened_types); //convert the _CROCOS_FLATTENED_TYPES type into a static array

    using _sorted_type = uint64_t[_crocos_obj_base_count];
//#ifdef CORE_LINKED_WITH_KERNEL
    __attribute__((used)) static _sorted_type _crocos_sorted_parents;

    __attribute__((used)) static void _crocos_presort() {
        memcpy(_crocos_sorted_parents, _crocos_const_flattened_ids, _crocos_obj_base_count * sizeof(uint64_t));
        algorithm::sort(_crocos_sorted_parents);
    }

    inline static void (*_crocos_presort_init)(void) __attribute__((used, section(".crocos_presort_array"))) = _crocos_presort;
//#else
    //inline static _sorted_type _crocos_sorted_parents{_crocos_const_flattened_ids};
//#endif


    uint64_t type_id() override {
        return TypeID_v<T>;
    }

    const char* type_name() override {
        return TypeName<T>::name();
    }

    bool instanceof(uint64_t id) const override {
        const auto& arr = _crocos_sorted_parents;
        size_t left = 0, right = _crocos_obj_base_count;
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (arr[mid] == id) return true;
            if (arr[mid] < id) left = mid + 1;
            else right = mid;
        }
        return false;
    }
};

template <typename T, typename... Bases>
typename Object<T, Bases...>::_sorted_type Object<T, Bases...>::_crocos_sorted_parents = {};

void presort_object_parent_lists();

#endif //OBJECT_H
