//
// Created by Spencer Martin on 7/18/25.
//

#ifndef ITERATOR_H
#define ITERATOR_H

template<typename It>
class IteratorRange {
public:
    using Iterator = It;

    IteratorRange(It begin, It end) : b(begin), e(end) {}

    It begin() const { return b; }
    It end()   const { return e; }

private:
    It b, e;
};

// Concept for types that provide iteration through begin() and end()
template<typename T>
concept Iterable = requires(T t) {
    { t.begin() } -> convertible_to<decltype(t.end())>;
    { t.end() };
};

#endif //ITERATOR_H
