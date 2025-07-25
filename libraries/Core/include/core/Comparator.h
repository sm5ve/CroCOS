//
// Created by Spencer Martin on 4/12/25.
//

#ifndef CROCOS_COMPARATOR_H
#define CROCOS_COMPARATOR_H

#include "utility.h"

template <typename T>
struct DefaultComparator{
    bool operator() (const T& a, const T& b) const{
        static_assert(comparable_less_than<T>, "Cannot use default comparator for type with no '<' operator");
        return a < b;
    }
};

template <typename Comp>
struct ReversedComparator {
    template <typename T>
    bool operator() (const T& a, const T& b) const {
        return Comp{}(b, a);
    }
};

// Convenience factory for default comparator
template <typename T>
using ReversedDefaultComparator = ReversedComparator<DefaultComparator<T>>;

#endif //CROCOS_COMPARATOR_H
