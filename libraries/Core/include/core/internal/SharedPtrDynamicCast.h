//
// Created by Spencer Martin on 8/24/25.
//
#if defined(OBJECT_H) && defined(CROCOS_SMARTPOINTER_H)
#ifndef CROCOS_SHAREDPTRDYNAMICCAST_H
#define CROCOS_SHAREDPTRDYNAMICCAST_H
template<typename U, typename V>
SharedPtr<U> crocos_dynamic_cast(const SharedPtr<V>& ptr){
    if (!ptr) {
        return SharedPtr<U>();
    }

    // Use CRClass instanceof for type checking
    if (!ptr.typed_ptr->instanceof(TypeID_v<U>)) {
        return SharedPtr<U>();
    }

    // Use crocos_dynamic_cast for the conversion
    U* new_ptr = crocos_dynamic_cast<U*>(ptr.typed_ptr);
    if (!new_ptr) {
        return SharedPtr<U>();
    }

    return SharedPtr<U>(ptr, new_ptr);
}
#endif
#endif //CROCOS_SHAREDPTRDYNAMICCAST_H