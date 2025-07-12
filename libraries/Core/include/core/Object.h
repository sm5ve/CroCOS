//
// Created by Spencer Martin on 5/25/25.
//

#ifndef OBJECT_H
#define OBJECT_H

#include <core/utility.h>
#include <core/TypeTraits.h>
#include <core/algo/sort.h>
#include <core/ds/Optional.h>
#include "preprocessor.h"

class ObjectBase {
public:
    virtual ~ObjectBase() = default;
    [[nodiscard]]
    virtual uint64_t type_id() const = 0;
    [[nodiscard]]
    virtual const char* type_name() const = 0;
    [[nodiscard]]
    virtual bool instanceof(uint64_t) const = 0;

    template <typename T>
    [[nodiscard]]
    bool instanceof() const {
        return instanceof(TypeID_v<T>);
    }
protected:
    virtual Optional<int64_t> getOffset(uint64_t id) = 0;
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

template <typename Derived, typename Base>
int64_t _computeCastingOffset() requires StaticCastable<Derived*, Base*> {
    // Use null pointer arithmetic instead of fake object addresses
    constexpr uintptr_t null_derived = 0x1000;  // Use a non-zero "fake" address
    Derived* derived = reinterpret_cast<Derived*>(null_derived);
    Base* base = static_cast<Base*>(derived);
    int64_t offset = reinterpret_cast<uintptr_t>(base) - reinterpret_cast<uintptr_t>(derived);
    return offset;
}

struct _InheritanceInfo {
    uint64_t id;
    int64_t offset;
    bool supports_dynamic_cast;
};

struct _InheritanceInfoComparator{
    bool operator() (const _InheritanceInfo& a, const _InheritanceInfo& b) const{
        return a.id < b.id;
    }
};

template<typename Base>
constexpr _InheritanceInfo _computeInheritanceInfo(){
    return _InheritanceInfo {.id = TypeID_v<Base>, .offset = 0, .supports_dynamic_cast = false};
}

template <typename T, typename... Bases>
class _ObjectInheritanceImpl {
private:
    template<typename... Ts>
    static constexpr _InheritanceInfo const (&make_type_id_array(type_list<Ts...>))[sizeof...(Ts)] {
    static constexpr _InheritanceInfo arr[sizeof...(Ts)] = { _computeInheritanceInfo<Ts>()... };
    return arr;
    }

    template<typename... Ts>
    static constexpr size_t get_type_array_size(type_list<Ts...>){
        return sizeof...(Ts);
    }
public:
    using _CROCOS_FLATTENED_TYPES = typename unique_type_list<
        typename concat<type_list<T, ObjectBase>, typename FlattenedObjectBaseList<Bases...>::type>::type
    >::type;;
    static _CROCOS_FLATTENED_TYPES _crocos_flattened_types;

    static const size_t _crocos_obj_base_count = get_type_array_size(_crocos_flattened_types);
    static constexpr auto _crocos_const_flattened_ids = make_type_id_array(_crocos_flattened_types); //convert the _CROCOS_FLATTENED_TYPES type into a static array

    using _sorted_type = _InheritanceInfo[_crocos_obj_base_count];
//#ifdef CORE_LINKED_WITH_KERNEL
    __attribute__((used)) static _sorted_type _crocos_sorted_parents;

    template <typename R, typename... Rs>
    static void _crocos_populate_offset(type_list<R, Rs...>, int index) requires StaticCastable<T*, R*>{
        _crocos_sorted_parents[index].offset = _computeCastingOffset<T, R>();
        _crocos_sorted_parents[index].supports_dynamic_cast = true;

        if constexpr (sizeof...(Rs) > 0) {
            _crocos_populate_offset(type_list<Rs...>{}, index + 1);
        }
    }

    template <typename R, typename... Rs>
    static void _crocos_populate_offset(type_list<R, Rs...>, int index) requires (!StaticCastable<T*, R*>){
        _crocos_sorted_parents[index].offset = 0;
        _crocos_sorted_parents[index].supports_dynamic_cast = false;

        if constexpr (sizeof...(Rs) > 0) {
            _crocos_populate_offset(type_list<Rs...>{}, index + 1);
        }
    }

    // Base case for empty type list
    static void _crocos_populate_offset(type_list<>, int) {

    }

    __attribute__((used)) static void _crocos_presort(){
        memcpy(_crocos_sorted_parents, _crocos_const_flattened_ids, sizeof(_InheritanceInfo) * _crocos_obj_base_count);
        _crocos_populate_offset(_crocos_flattened_types, 0);
        algorithm::sort(_crocos_sorted_parents, _InheritanceInfoComparator{});
    }

    inline static void (*_crocos_presort_init)(void) __attribute__((used, section(".crocos_presort_array"))) = _crocos_presort;
//#else
    //inline static _sorted_type _crocos_sorted_parents{_crocos_const_flattened_ids};
//#endif


    static uint64_t type_id() {
        return TypeID_v<T>;
    }

    static const char* type_name() {
        return TypeName<T>::name();
    }

    static bool instanceof(uint64_t id) {
        const auto& arr = _crocos_sorted_parents;
        size_t left = 0, right = _crocos_obj_base_count;
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (arr[mid].id == id) return true;
            if (arr[mid].id < id) left = mid + 1;
            else right = mid;
        }
        return false;
    }

    static Optional<int64_t> getOffset(uint64_t id) {
        const auto& arr = _crocos_sorted_parents;
        size_t left = 0, right = _crocos_obj_base_count;
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (arr[mid].id == id) {
                if (arr[mid].supports_dynamic_cast)
                    return arr[mid].offset;
                return {};
            }
            if (arr[mid].id < id) left = mid + 1;
            else right = mid;
        }
        return {};
    }
};

template <typename T, typename... Bases>
typename _ObjectInheritanceImpl<T, Bases...>::_sorted_type _ObjectInheritanceImpl<T, Bases...>::_crocos_sorted_parents = {};

#define _CRClass_IMPL(Name, AuxName, ...) \
class Name; \
class AuxName : public virtual ObjectBase __VA_OPT__(,) __VA_ARGS__ { \
using _impl = _ObjectInheritanceImpl<Name __VA_OPT__(, STRIP(__VA_ARGS__))>; \
public: \
static _impl::_CROCOS_FLATTENED_TYPES _crocos_flattened_types; \
virtual uint64_t type_id() const override { return _impl::type_id(); } \
virtual const char* type_name() const override { return _impl::type_name(); } \
virtual bool instanceof(uint64_t id) const override { return _impl::instanceof(id); } \
virtual Optional<int64_t> getOffset(uint64_t id) override { return _impl::getOffset(id); }\
};\
class Name : public AuxName

#define CRClass(Name, ...) \
_CRClass_IMPL(Name, _CRClass_IMPL_ ## Name ## _ ## __LINE__ ## _ ## __COUNTER__, __VA_ARGS__)

template<typename Type>
concept DynamicCastable = requires(Type from) {
    from.getOffset(0ul);
};

//Not guaranteed to work for virtual inheritance
template <typename Dest, typename Source>
Dest crocos_dynamic_cast(Source s) requires (is_pointer_v<Dest> && is_pointer_v<Source>)
{
    auto destOffset = s -> getOffset(TypeID_v<typename remove_pointer<Dest>::type>);
    auto sourceOffset = s -> getOffset(TypeID_v<typename remove_pointer<Source>::type>);
    if (destOffset.occupied() && sourceOffset.occupied()) {
        int64_t offset = *destOffset - *sourceOffset;
        auto ptr = reinterpret_cast<char*>(s);
        ptr += offset;
        return reinterpret_cast<Dest>(ptr);
    }

    return nullptr;
}

void presort_object_parent_lists();

#endif //OBJECT_H
