//
// Created by Spencer Martin on 7/18/25.
//

#ifndef ITERATOR_H
#define ITERATOR_H

// Concept for types that provide iteration through begin() and end()
template<typename T>
concept Iterable = requires(T t) {
    { t.end() };
    { t.begin() } -> convertible_to<decltype(t.end())>;
};

template<typename Itr, typename ReturnType>
concept IterableWithIteratorType = requires(Itr t) {
    { t.end() } -> convertible_to<ReturnType>;
    { t.begin() } -> convertible_to<ReturnType>;
};

template<typename Itr, typename ReturnType>
concept IterableWithValueType = requires(Itr t) {
    { *t.end() } -> convertible_to<ReturnType>;
    { *t.begin() } -> convertible_to<ReturnType>;
};

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

template<typename Filter, typename It>
concept IteratorFilter = requires(Filter f, It it)
{
    Iterable<It>;
    {f(*it)} -> convertible_to<bool>;
};

template<typename Filter, typename It>
requires IteratorFilter<Filter, It>
class FilteredIterator {
    It itBegin;
    It itEnd;
    Filter filter;
public:
    //template<typename ItRange>
    //requires IterableWithType<ItRange, It>
    //FilteredIterator(ItRange i, Filter f) : itBegin(i.begin()), itEnd(i.end()), filter(f) {}

    FilteredIterator(It begin, It end, Filter f) : itBegin(begin), itEnd(end), filter(f) {}

    class IteratorImpl {
        It it;
        It end;
        Filter filter;
    private:
        void advanceToValidState() {
            while (it != end && !filter(*it)) {
                ++it;
            }
        }
    public:
        IteratorImpl(It i, It e, Filter f) : it(i), end(e), filter(f) {advanceToValidState();}
        bool operator!=(const IteratorImpl& other) const { return it != other.it; }
        auto operator*() const { return *it; }
        IteratorImpl& operator++() { do {
            ++it;
        } while ((it != end) && !filter(*it)); return *this; }
    };

    IteratorImpl begin() const { return IteratorImpl(itBegin, itEnd, filter); }
    IteratorImpl end() const { return IteratorImpl(itEnd, itEnd, filter); }
};

#endif //ITERATOR_H
