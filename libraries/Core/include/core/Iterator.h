//
// Created by Spencer Martin on 7/18/25.
//

#ifndef ITERATOR_H
#define ITERATOR_H

template<typename It>
class IteratorRange {
public:
    IteratorRange(It begin, It end) : b(begin), e(end) {}

    It begin() const { return b; }
    It end()   const { return e; }

private:
    It b, e;
};

#endif //ITERATOR_H
