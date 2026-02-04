//
// Created by Spencer Martin on 1/31/26.
//

#ifndef CROCOS_PERMUTATION_H
#define CROCOS_PERMUTATION_H

#include <stddef.h>
#include <assert.h>
#include <core/TypeTraits.h>
#include <core/utility.h>

#define PERM_SKIP_EXPENSIVE_ASSERT

enum PermutationSparsity {
    SPARSE,
    DENSE
};

template <size_t bits, PermutationSparsity sparsity = DENSE>
class Permutation{
public:
    constexpr static size_t requiredBits = (sparsity == DENSE ? 0 : 1) + bits;
    using IndexType = SmallestUInt_t<bits>;

    struct ElementIndex{
        IndexType index;
        bool operator==(const ElementIndex&) const = default;
    };

    struct PositionIndex{
        IndexType index;
        bool operator==(const PositionIndex&) const = default;
    };

    constexpr static IndexType INVALID = static_cast<IndexType>(-1);
private:
    template <size_t b, PermutationSparsity s>
    friend class PermutationWindow;

    struct Empty{};
    struct NoIndex{}; //Tag type for optional prefix/suffix in runtime rotate

    template<bool condition, typename T>
    using OptionallyPresent = conditional_t<condition, T, Empty>;

    IndexType* forwardBuffer;
    IndexType* backwardBuffer;

    OptionallyPresent<sparsity == SPARSE, size_t> _fwCap;
    size_t bwCap;

    constexpr size_t fwCap() const{
        if constexpr (sparsity == DENSE){
            return bwCap;
        } else {
            return _fwCap;
        }
    }

    IndexType& forwardEntry(ElementIndex index){
        assert(index.index < bwCap, "Index is out of bounds");
        return forwardBuffer[backwardBuffer[index.index]];
    }

    IndexType& backwardEntry(ElementIndex index){
        assert(index.index < bwCap, "Index is out of bounds");
        return backwardBuffer[index.index];
    }

    IndexType& forwardEntry(PositionIndex index){
        assert(index.index < fwCap(), "Index is out of bounds");
        return forwardBuffer[index.index];
    }

    IndexType& backwardEntry(PositionIndex index) {
        assert(index.index < fwCap(), "Index is out of bounds");
        return backwardBuffer[forwardBuffer[index.index]];
    }

    template <typename PrefixT, typename SuffixT>
    void rotateRightImpl(PrefixT prefix, PositionIndex* arr, size_t arrLen, SuffixT suffix,
                         IndexType offset, IndexType width) {
        constexpr bool hasPrefix = !is_same_v<PrefixT, NoIndex>;
        constexpr bool hasSuffix = !is_same_v<SuffixT, NoIndex>;

        size_t N = arrLen + (hasPrefix ? 1 : 0) + (hasSuffix ? 1 : 0);
        if (N <= 1) return;

        //Helper to adjust PositionIndex with bounds check and offset
        auto adjust = [&]<typename T>(T& t) {
            if constexpr (is_same_v<T, PositionIndex>) {
                assert(t.index < width, "Index out of bounds");
                t.index += offset;
            }
        };

        //Helper to restore PositionIndex (subtract offset)
        auto unadjust = [&](PositionIndex& t) {
            t.index -= offset;
        };

        //Apply offset to prefix/suffix and array elements
        if constexpr (hasPrefix) adjust(prefix);
        if constexpr (hasSuffix) adjust(suffix);
        for (size_t i = 0; i < arrLen; i++) adjust(arr[i]);

        //VLA allocation on the stack
        IndexType* fwPtrs[N];
        IndexType* bwPtrs[N];

        size_t idx = 0;
        if constexpr (hasPrefix) {
            fwPtrs[idx] = &forwardEntry(prefix);
            bwPtrs[idx] = &backwardEntry(prefix);
            idx++;
        }
        for (size_t i = 0; i < arrLen; i++) {
            fwPtrs[idx] = &forwardEntry(arr[i]);
            bwPtrs[idx] = &backwardEntry(arr[i]);
            idx++;
        }
        if constexpr (hasSuffix) {
            fwPtrs[idx] = &forwardEntry(suffix);
            bwPtrs[idx] = &backwardEntry(suffix);
        }

        //Check for repeated elements
        const auto unique = [&] {
#ifdef PERM_SKIP_EXPENSIVE_ASSERT
            if (N > 5) return true;
#endif
            for (size_t i = 0; i < N-1; i++) {
                for (size_t j = i + 1; j < N; j++) {
                    if (*fwPtrs[i] == *fwPtrs[j]) return false;
                }
            }
            return true;
        };
        assert(unique(), "Repeated element in rotateRight");

        IndexType lastFw = *fwPtrs[N-1];
        IndexType firstBw = *bwPtrs[0];

        //Forward buffer rotates RIGHT
        for (size_t i = N - 1; i > 0; i--) {
            *fwPtrs[i] = *fwPtrs[i-1];
        }
        *fwPtrs[0] = lastFw;

        //Backward buffer rotates LEFT (opposite direction)
        for (size_t i = 0; i < N - 1; i++) {
            *bwPtrs[i] = *bwPtrs[i+1];
        }
        *bwPtrs[N-1] = firstBw;

        //Restore original indices in the array
        for (size_t i = 0; i < arrLen; i++) unadjust(arr[i]);
    }

    template <typename PrefixT, typename SuffixT>
    void rotateLeftImpl(PrefixT prefix, PositionIndex* arr, size_t arrLen, SuffixT suffix,
                        IndexType offset, IndexType width) {
        constexpr bool hasPrefix = !is_same_v<PrefixT, NoIndex>;
        constexpr bool hasSuffix = !is_same_v<SuffixT, NoIndex>;

        size_t N = arrLen + (hasPrefix ? 1 : 0) + (hasSuffix ? 1 : 0);
        if (N <= 1) return;

        //Helper to adjust PositionIndex with bounds check and offset
        auto adjust = [&]<typename T>(T& t) {
            if constexpr (is_same_v<T, PositionIndex>) {
                assert(t.index < width, "Index out of bounds");
                t.index += offset;
            }
        };

        //Helper to restore PositionIndex (subtract offset)
        auto unadjust = [&](PositionIndex& t) {
            t.index -= offset;
        };

        //Apply offset to prefix/suffix and array elements
        if constexpr (hasPrefix) adjust(prefix);
        if constexpr (hasSuffix) adjust(suffix);
        for (size_t i = 0; i < arrLen; i++) adjust(arr[i]);

        //VLA allocation on the stack
        IndexType* fwPtrs[N];
        IndexType* bwPtrs[N];

        size_t idx = 0;
        if constexpr (hasPrefix) {
            fwPtrs[idx] = &forwardEntry(prefix);
            bwPtrs[idx] = &backwardEntry(prefix);
            idx++;
        }
        for (size_t i = 0; i < arrLen; i++) {
            fwPtrs[idx] = &forwardEntry(arr[i]);
            bwPtrs[idx] = &backwardEntry(arr[i]);
            idx++;
        }
        if constexpr (hasSuffix) {
            fwPtrs[idx] = &forwardEntry(suffix);
            bwPtrs[idx] = &backwardEntry(suffix);
        }

        //Check for repeated elements
        const auto unique = [&] {
#ifdef PERM_SKIP_EXPENSIVE_ASSERT
            if (N > 5) return true;
#endif
            for (size_t i = 0; i < N-1; i++) {
                for (size_t j = i + 1; j < N; j++) {
                    if (*fwPtrs[i] == *fwPtrs[j]) return false;
                }
            }
            return true;
        };
        assert(unique(), "Repeated element in rotateLeft");

        IndexType firstFw = *fwPtrs[0];
        IndexType lastBw = *bwPtrs[N-1];

        //Forward buffer rotates LEFT
        for (size_t i = 0; i < N - 1; i++) {
            *fwPtrs[i] = *fwPtrs[i+1];
        }
        *fwPtrs[N-1] = firstFw;

        //Backward buffer rotates RIGHT (opposite direction)
        for (size_t i = N - 1; i > 0; i--) {
            *bwPtrs[i] = *bwPtrs[i-1];
        }
        *bwPtrs[0] = lastBw;

        //Restore original indices in the array
        for (size_t i = 0; i < arrLen; i++) unadjust(arr[i]);
    }

public:
    void reset(){
        for (IndexType i = 0; i < bwCap; i++) {
            forwardBuffer[i] = i;
            backwardBuffer[i] = i;
        }
        for (IndexType i = bwCap; i < fwCap(); i++) {
            forwardBuffer[i] = INVALID;
        }
    }

    Permutation(IndexType* fb, IndexType* bb, size_t cap) requires (sparsity == DENSE){
        forwardBuffer = fb;
        backwardBuffer = bb;
        bwCap = cap;
    }

    Permutation(IndexType* fb, size_t fbCap, IndexType* bb, size_t bbCap) requires (sparsity == SPARSE){
        forwardBuffer = fb;
        backwardBuffer = bb;
        bwCap = bbCap;
        _fwCap = fbCap;
        assert(_fwCap >= bwCap, "Forward buffer must be at least as large as backward buffer");
    }

    Permutation(const Permutation& other) = default;
    Permutation& operator=(const Permutation& other) = default;

    template <typename T>
    constexpr static bool is_index_type_v = IsSame<T, PositionIndex> || IsSame<T, ElementIndex>;

    template <typename... Ts>
    constexpr static bool are_index_types_v = (is_index_type_v<Ts> && ...);

    template <typename T, typename S>
    void swap(T t, S s) requires (are_index_types_v<T, S>){
        //Retain references ahead of time, since forwardEntry and backwardEntry may give inaccurate results after
        //swapping entries in one buffer and before swapping the other
        auto& tfw = forwardEntry(t);
        auto& sfw = forwardEntry(s);
        auto& tbw = backwardEntry(t);
        auto& sbw = backwardEntry(s);

        //If we're swapping an element with itself, this is a noop.
        if (tfw == sfw) {
            return;
        }

        ::swap(tfw, sfw);
        ::swap(tbw, sbw);
    }

    template <typename... Ts>
    //Rotates an arbitrary number of elements right. If there is a repeated element, the method asserts.
    void rotateRight(Ts... ts) requires (are_index_types_v<Ts...>){
        constexpr size_t N = sizeof...(Ts);
        if constexpr (N <= 1) return;

        //Retain references ahead of time, since forwardEntry and backwardEntry may give inaccurate results after
        //modifying entries in one buffer and before modifying the other
        IndexType* fwPtrs[] = { &forwardEntry(ts)... };
        IndexType* bwPtrs[] = { &backwardEntry(ts)... };

        //Check for repeated elements
        const auto unique = [&] {
#ifdef PERM_SKIP_EXPENSIVE_ASSERT
            if constexpr (N > 5) return true; // Skip the check when this becomes expensive.
#endif
            for (size_t i = 0; i < N-1; i++) {
                for (size_t j = i + 1; j < N; j++) {
                    if (*fwPtrs[i] == *fwPtrs[j]) return false;
                }
            }
            return true;
        };
        assert(unique(), "Repeated element in rotateRight");

        //Save the last forward value and first backward value
        auto lastFw = *fwPtrs[N-1];
        auto firstBw = *bwPtrs[0];

        //Forward buffer rotates RIGHT
        for (size_t i = N - 1; i > 0; i--) {
            *fwPtrs[i] = *fwPtrs[i-1];
        }
        *fwPtrs[0] = lastFw;

        //Backward buffer rotates LEFT (opposite direction)
        for (size_t i = 0; i < N - 1; i++) {
            *bwPtrs[i] = *bwPtrs[i+1];
        }
        *bwPtrs[N-1] = firstBw;
    }

    template <typename... Ts>
    //Rotates an arbitrary number of elements left. If there is a repeated element, the method asserts.
    void rotateLeft(Ts... ts) requires (are_index_types_v<Ts...>){
        constexpr size_t N = sizeof...(Ts);
        if constexpr (N <= 1) return;

        //Retain references ahead of time, since forwardEntry and backwardEntry may give inaccurate results after
        //modifying entries in one buffer and before modifying the other
        IndexType* fwPtrs[] = { &forwardEntry(ts)... };
        IndexType* bwPtrs[] = { &backwardEntry(ts)... };

        //Check for repeated elements
        const auto unique = [&] {
#ifdef PERM_SKIP_EXPENSIVE_ASSERT
            if constexpr (N > 5) return true; // Skip the check when this becomes expensive.
#endif
            for (size_t i = 0; i < N-1; i++) {
                for (size_t j = i + 1; j < N; j++) {
                    if (*fwPtrs[i] == *fwPtrs[j]) return false;
                }
            }
            return true;
        };
        assert(unique(), "Repeated element in rotateLeft");

        //Save the first forward value and last backward value
        auto firstFw = *fwPtrs[0];
        auto lastBw = *bwPtrs[N-1];

        //Forward buffer rotates LEFT
        for (size_t i = 0; i < N - 1; i++) {
            *fwPtrs[i] = *fwPtrs[i+1];
        }
        *fwPtrs[N-1] = firstFw;

        //Backward buffer rotates RIGHT (opposite direction)
        for (size_t i = N - 1; i > 0; i--) {
            *bwPtrs[i] = *bwPtrs[i-1];
        }
        *bwPtrs[0] = lastBw;
    }

    //Runtime-length rotateRight overloads: array only
    void rotateRight(PositionIndex* arr, size_t len) {
        rotateRightImpl(NoIndex{}, arr, len, NoIndex{}, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateRight overloads: prefix + array
    template <typename T>
    void rotateRight(T prefix, PositionIndex* arr, size_t len) {
        rotateRightImpl(prefix, arr, len, NoIndex{}, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateRight overloads: array + suffix
    template <typename T>
    void rotateRight(PositionIndex* arr, size_t len, T suffix) {
        rotateRightImpl(NoIndex{}, arr, len, suffix, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateRight overloads: prefix + array + suffix
    template <typename T, typename S>
    void rotateRight(T prefix, PositionIndex* arr, size_t len, S suffix) {
        rotateRightImpl(prefix, arr, len, suffix, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateLeft overloads: array only
    void rotateLeft(PositionIndex* arr, size_t len) {
        rotateLeftImpl(NoIndex{}, arr, len, NoIndex{}, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateLeft overloads: prefix + array
    template <typename T>
    void rotateLeft(T prefix, PositionIndex* arr, size_t len) {
        rotateLeftImpl(prefix, arr, len, NoIndex{}, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateLeft overloads: array + suffix
    template <typename T>
    void rotateLeft(PositionIndex* arr, size_t len, T suffix) {
        rotateLeftImpl(NoIndex{}, arr, len, suffix, 0, static_cast<IndexType>(fwCap()));
    }

    //Runtime-length rotateLeft overloads: prefix + array + suffix
    template <typename T, typename S>
    void rotateLeft(T prefix, PositionIndex* arr, size_t len, S suffix) {
        rotateLeftImpl(prefix, arr, len, suffix, 0, static_cast<IndexType>(fwCap()));
    }

    [[nodiscard]] IndexType positionOf(const ElementIndex index) {
        return backwardEntry(index);
    }

    [[nodiscard]] IndexType elementAt(const PositionIndex index) {
        return forwardEntry(index);
    }

    [[nodiscard]] IndexType operator[](const PositionIndex index) {
        return elementAt(index);
    }

    [[nodiscard]] size_t elementCount() const {
        return bwCap;
    }

    [[nodiscard]] size_t positionCount() const {
        return fwCap();
    }

    //Validates that forward and backward buffers are consistent.
    //Returns true if valid, false if corruption detected.
    [[nodiscard]] bool validate() const {
        //For each position with a valid element, check that the backward buffer points back
        for (size_t pos = 0; pos < fwCap(); pos++) {
            IndexType elem = forwardBuffer[pos];
            if (elem == INVALID) continue;  //Skip invalid entries (sparse permutation)

            if (elem >= bwCap) return false;  //Element out of range

            //backwardBuffer[elem] should equal pos
            if (backwardBuffer[elem] != pos) return false;
        }

        //For each element, check that the forward buffer points back
        for (size_t elem = 0; elem < bwCap; elem++) {
            IndexType pos = backwardBuffer[elem];
            if (pos >= fwCap()) return false;  //Position out of range

            //forwardBuffer[pos] should equal elem
            if (forwardBuffer[pos] != elem) return false;
        }

        //For dense permutations, verify no duplicates in forward buffer
        if constexpr (sparsity == DENSE) {
            for (size_t i = 0; i < bwCap; i++) {
                for (size_t j = i + 1; j < bwCap; j++) {
                    if (forwardBuffer[i] == forwardBuffer[j]) return false;
                }
            }
        }

        return true;
    }

    //Debug assertion version - asserts on failure with a message
    void assertValid(const char* context = "Permutation validation") const {
        assert(validate(), context);
    }
};

template <size_t bits, PermutationSparsity sparsity>
class PermutationWindow {
    Permutation<bits, sparsity> permutation;
    size_t offset;
    size_t width;
    public:
    PermutationWindow(Permutation<bits, sparsity>& perm) : permutation(perm) {
        offset = 0;
        width = permutation.positionCount();
    }

    PermutationWindow(Permutation<bits, sparsity>& perm, const size_t off, const size_t w) : permutation(perm) {
        offset = off;
        width = w;
        assert(off + w <= permutation.positionCount(), "Window goes out of bounds");
    }

    PermutationWindow(const PermutationWindow&) = default;
    PermutationWindow(PermutationWindow&&) = default;

    PermutationWindow& operator=(const PermutationWindow& other) {
        offset = other.offset;
        width = other.width;
        permutation = other.permutation;
        return *this;
    }

    using ElementIndex = typename Permutation<bits, sparsity>::ElementIndex;
    using PositionIndex = typename Permutation<bits, sparsity>::PositionIndex;
    using IndexType = typename Permutation<bits, sparsity>::IndexType;
    using NoIndex = typename Permutation<bits, sparsity>::NoIndex;

    template <typename T, typename S>
    void swap(T t, S s) {
        if constexpr(is_same_v<T, PositionIndex>) {
            assert(t.index < width, "Index out of bounds for permutation window");
            t.index += offset;
        }
        if constexpr(is_same_v<S, PositionIndex>) {
            assert(s.index < width, "Index out of bounds for permutation window");
            s.index += offset;
        }
        permutation.swap(t, s);
    }

    template <typename T>
    constexpr static bool is_index_type_v = is_same_v<T, PositionIndex> || is_same_v<T, ElementIndex>;

    template <typename... Ts>
    constexpr static bool are_index_types_v = (is_index_type_v<Ts> && ...);

    template <typename... Ts>
    void rotateRight(Ts... ts) requires (are_index_types_v<Ts...>) {
        auto adjust = [&]<typename T>(T& t) {
            if constexpr(is_same_v<T, PositionIndex>) {
                assert(t.index < width, "Index out of bounds for permutation window");
                t.index += offset;
            }
        };
        (adjust(ts), ...);
        permutation.rotateRight(ts...);
    }

    template <typename... Ts>
    void rotateLeft(Ts... ts) requires (are_index_types_v<Ts...>) {
        auto adjust = [&]<typename T>(T& t) {
            if constexpr(is_same_v<T, PositionIndex>) {
                assert(t.index < width, "Index out of bounds for permutation window");
                t.index += offset;
            }
        };
        (adjust(ts), ...);
        permutation.rotateLeft(ts...);
    }

    //Runtime-length rotateRight overloads: array only
    void rotateRight(PositionIndex* arr, size_t len) {
        permutation.rotateRightImpl(NoIndex{}, arr, len, NoIndex{},
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateRight overloads: prefix + array
    template <typename T>
    void rotateRight(T prefix, PositionIndex* arr, size_t len) {
        permutation.rotateRightImpl(prefix, arr, len, NoIndex{},
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateRight overloads: array + suffix
    template <typename T>
    void rotateRight(PositionIndex* arr, size_t len, T suffix) {
        permutation.rotateRightImpl(NoIndex{}, arr, len, suffix,
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateRight overloads: prefix + array + suffix
    template <typename T, typename S>
    void rotateRight(T prefix, PositionIndex* arr, size_t len, S suffix) {
        permutation.rotateRightImpl(prefix, arr, len, suffix,
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateLeft overloads: array only
    void rotateLeft(PositionIndex* arr, size_t len) {
        permutation.rotateLeftImpl(NoIndex{}, arr, len, NoIndex{},
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateLeft overloads: prefix + array
    template <typename T>
    void rotateLeft(T prefix, PositionIndex* arr, size_t len) {
        permutation.rotateLeftImpl(prefix, arr, len, NoIndex{},
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateLeft overloads: array + suffix
    template <typename T>
    void rotateLeft(PositionIndex* arr, size_t len, T suffix) {
        permutation.rotateLeftImpl(NoIndex{}, arr, len, suffix,
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    //Runtime-length rotateLeft overloads: prefix + array + suffix
    template <typename T, typename S>
    void rotateLeft(T prefix, PositionIndex* arr, size_t len, S suffix) {
        permutation.rotateLeftImpl(prefix, arr, len, suffix,
            static_cast<IndexType>(offset), static_cast<IndexType>(width));
    }

    [[nodiscard]] bool inWindow(const ElementIndex index) {
        auto position = permutation.positionOf(index);
        return (position >= offset) && (position < width + offset);
    }

    [[nodiscard]] IndexType positionOf(const ElementIndex index) {
        assert(inWindow(index), "Element not in the window");
        return permutation.positionOf(index) - offset;
    }

    [[nodiscard]] IndexType elementAt(PositionIndex index) {
        assert(index.index < width, "Index out of bounds for permutation window");
        index.index += offset;
        return permutation.elementAt(index);
    }

    [[nodiscard]] IndexType operator[](const PositionIndex index) {
        return elementAt(index);
    }

    [[nodiscard]] bool validate() const {
        return permutation.validate();
    }

    void assertValid(const char* context = "PermutationWindow validation") const {
        permutation.assertValid(context);
    }

    [[nodiscard]] size_t windowSize() const {
        return width;
    }
};

template <size_t bits, PermutationSparsity sparsity = DENSE, bool owning = false>
class BucketedSet {
public:
    using IndexType = typename PermutationWindow<bits, sparsity>::IndexType;
    using PositionIndex = typename PermutationWindow<bits, sparsity>::PositionIndex;
    using ElementIndex = typename PermutationWindow<bits, sparsity>::ElementIndex;
private:
    PermutationWindow<bits, sparsity> permutation;
    IndexType* bucketMarkers;
    size_t bucketCount;

public:
    // Construction
    BucketedSet(const PermutationWindow<bits, sparsity>& perm, const size_t numBuckets) requires (owning) : permutation(perm){
        bucketCount = numBuckets;
        bucketMarkers = new IndexType[bucketCount - 1];
    }

    BucketedSet(const PermutationWindow<bits, sparsity>& perm, IndexType* markers, const size_t numBuckets) requires (!owning) : permutation(perm){
        bucketCount = numBuckets;
        bucketMarkers = markers;
    }

    ~BucketedSet() {
        if constexpr (owning) {
            delete[] bucketMarkers;
        }
    }

    // Query operations
    bool contains(ElementIndex e) {
        return permutation.inWindow(e);
    }

    size_t getBucket(ElementIndex e) {
        assert(contains(e), "Element is not in this set.");
        auto index = permutation.positionOf(e);
        // Only one bucket
        if (bucketCount == 1)
            return 0;

        // Below first marker
        if (index < bucketMarkers[0])
            return 0;

        // At or beyond last marker
        if (index >= bucketMarkers[bucketCount - 2])
            return bucketCount - 1;

        // Binary search in [0, bucketCount-2)
        size_t left = 0;
        size_t right = bucketCount - 2; // last valid marker index

        while (left < right)
        {
            size_t mid = left + (right - left) / 2;

            if (index < bucketMarkers[mid])
            {
                right = mid;
            }
            else
            {
                left = mid + 1;
            }
        }

        return left;
    }

    size_t bucketSize(const size_t bucket){
        assert(bucket < bucketCount, "Bucket out of bounds");
        if (bucketCount == 1) {
            return permutation.windowSize();
        }
        else if (bucket == 0) {
            return bucketMarkers[0];
        } else if (bucket == bucketCount - 1) {
            return permutation.windowSize() - bucketMarkers[bucketCount - 2];
        } else {
            return bucketMarkers[bucket] - bucketMarkers[bucket - 1];
        }
    }

    bool bucketEmpty(const size_t bucket) {
        return bucketSize(bucket) == 0;
    }

    // Bucket boundaries (for iteration)
    PositionIndex bucketStart(const size_t bucket) {
        assert(bucket < bucketCount, "Bucket out of bounds");
        if (bucket == 0) {
            return PositionIndex{0};
        }
        return PositionIndex{bucketMarkers[bucket - 1]};
    }

    // One past last position
    PositionIndex bucketEnd(const size_t bucket) {
        assert(bucket < bucketCount, "Bucket out of bounds");
        if (bucket == bucketCount - 1) {
            return PositionIndex{static_cast<IndexType>(permutation.windowSize())};
        }
        return PositionIndex{bucketMarkers[bucket]};
    }

    // Access elements in buckets
    // First element (asserts non-empty)
    ElementIndex topOfBucket(const size_t bucket) {
        assert(!bucketEmpty(bucket), "Bucket is empty.");
        return ElementIndex{permutation.elementAt(bucketStart(bucket))};
    }
    // Last element (asserts non-empty)
    ElementIndex bottomOfBucket(const size_t bucket) {
        PositionIndex position = bucketEnd(bucket);
        position.index -= 1;
        return ElementIndex{permutation.elementAt(position)};
    }

    // Move elements between buckets
    void moveToBucket(ElementIndex e, size_t targetBucket) {
        assert(contains(e), "Element is not in this set.");
        assert(targetBucket < bucketCount, "Target bucket out of bounds");

        size_t currentBucket = getBucket(e);
        if (currentBucket == targetBucket) return;

        if (currentBucket < targetBucket) {
            // Moving to a higher bucket
            // Swap e to end of its current bucket
            PositionIndex ePos{permutation.positionOf(e)};
            PositionIndex endPos = bucketEnd(currentBucket);
            --endPos.index;
            if (ePos.index != endPos.index) {
                permutation.swap(ePos, endPos);
            }

            // Build array of boundary positions: end of each bucket from current to target-1
            // Deduplicate to handle empty intermediate buckets
            size_t maxCount = targetBucket - currentBucket;
            PositionIndex positions[maxCount];  // VLA
            size_t count = 0;

            for (size_t i = 0; i < maxCount; ++i) {
                PositionIndex pos = bucketEnd(currentBucket + i);
                --pos.index;
                // Add only if different from previous (handles empty buckets)
                if (count == 0 || positions[count - 1].index != pos.index) {
                    positions[count++] = pos;
                }
            }

            // Rotate left: e goes from first position to last position
            if (count > 1) {
                permutation.rotateLeft(positions, count);
            }

            // Decrement markers from currentBucket to targetBucket - 1
            for (size_t b = currentBucket; b < targetBucket; ++b) {
                --bucketMarkers[b];
            }
        } else {
            // Moving to a lower bucket
            // Swap e to start of its current bucket
            PositionIndex ePos{permutation.positionOf(e)};
            PositionIndex startPos = bucketStart(currentBucket);
            if (ePos.index != startPos.index) {
                permutation.swap(ePos, startPos);
            }

            // Build array of boundary positions: start of each bucket from current down to target
            // (include targetBucket so e ends up at the top of target)
            // Deduplicate to handle empty intermediate buckets
            size_t maxCount = currentBucket - targetBucket + 1;
            PositionIndex positions[maxCount];  // VLA
            size_t count = 0;

            for (size_t i = 0; i < maxCount; ++i) {
                PositionIndex pos = bucketStart(currentBucket - i);
                // Add only if different from previous (handles empty buckets)
                if (count == 0 || positions[count - 1].index != pos.index) {
                    positions[count++] = pos;
                }
            }

            // Rotate left: e goes from first position to last position
            if (count > 1) {
                permutation.rotateLeft(positions, count);
            }

            // Increment markers from targetBucket to currentBucket - 1
            for (size_t b = targetBucket; b < currentBucket; ++b) {
                ++bucketMarkers[b];
            }
        }
    }

    void moveToBucket(ElementIndex e, size_t expectedSource, size_t targetBucket) {
        assert(contains(e), "Set doesn't contain element");
        assert(getBucket(e) == expectedSource, "Element not in expected bucket");
        moveToBucket(e, targetBucket);
    }

    // Optimized transfers between adjacent buckets
    // Move bottom element of source to top of next bucket
    ElementIndex transferToNextBucket(const size_t sourceBucket) {
        assert(!bucketEmpty(sourceBucket), "Bucket is empty.");
        assert(sourceBucket != bucketCount - 1, "Cannot transfer to next bucket from top bucket");
        --bucketMarkers[sourceBucket];
        return topOfBucket(sourceBucket + 1);
    }
    // Move top element of source to bottom of prev bucket
    ElementIndex transferToPrevBucket(const size_t sourceBucket) {
        assert(!bucketEmpty(sourceBucket), "Bucket is empty.");
        assert(sourceBucket != 0, "Cannot transfer to previous bucket from bottom bucket");
        ++bucketMarkers[sourceBucket - 1];
        return bottomOfBucket(sourceBucket - 1);
    }

    // Initialization
    // Put all elements in topmost bucket
    void reset() {
        for (size_t i = 0; i < bucketCount - 1; i++) {
            bucketMarkers[i] = 0;
        }
    }
};

#endif //CROCOS_PERMUTATION_H