//
// Created by Spencer Martin on 3/28/26.
//

#ifndef CROCOS_ARRAY_H
#define CROCOS_ARRAY_H

#include <stddef.h>
#include "assert.h"
#include <core/utility.h>

// Helper: check that I is callable with exactly Rank size_t arguments and returns T
template <typename I, typename T, typename Seq>
struct ElementInitializerCheck : false_type {};

template <typename I, typename T, size_t... Is>
struct ElementInitializerCheck<I, T, index_sequence<Is...>> {
    static constexpr bool value = requires(I i) {
        { i(((void)Is, size_t{})...) } -> IsSame<T>;
    };
};

// I must be callable with Rank size_t indices (one per dimension) and return T
template <typename I, typename T, size_t Rank>
concept ElementInitializer = ElementInitializerCheck<I, T, make_index_sequence<Rank>>::value;

// -----------------------------------------------------------------------------------------
// DArray — Rank-dimensional array with extents determined at runtime
// -----------------------------------------------------------------------------------------
template <typename T, size_t Rank>
class DArray {
    static_assert(Rank > 0, "DArray requires Rank >= 1");

    T*     data_     = nullptr;
    size_t extents_[Rank] = {};
    size_t total_    = 0;

    // Row-major flat index from multi-dimensional indices
    size_t flatIndex(const size_t (&indices)[Rank]) const {
        size_t flat   = 0;
        size_t stride = 1;
        for (size_t i = Rank; i > 0; i--) {
            flat   += indices[i - 1] * stride;
            stride *= extents_[i - 1];
        }
        return flat;
    }

    // Decompose a linear index back into per-dimension indices
    void decomposeLinear(size_t linear, size_t (&indices)[Rank]) const {
        for (size_t i = Rank; i > 0; i--) {
            indices[i - 1]  = linear % extents_[i - 1];
            linear          /= extents_[i - 1];
        }
    }

    template <typename I, size_t... Is>
    static T invokeInit(I& init, const size_t (&indices)[Rank], index_sequence<Is...>) {
        return init(indices[Is]...);
    }

    void destroy() {
        if (data_) {
            for (size_t i = 0; i < total_; i++)
                data_[i].~T();
            operator delete(data_);
            data_  = nullptr;
            total_ = 0;
        }
    }

    template <typename... Sizes>
    void storeExtents(Sizes... sizes) {
        size_t idx = 0;
        ((extents_[idx++] = static_cast<size_t>(sizes)), ...);
        total_ = 1;
        for (size_t i = 0; i < Rank; i++) total_ *= extents_[i];
    }

public:
    // Construct each element by calling initializer(i0, i1, ..., iRank-1) for its index
    template <typename I, typename... Sizes>
    requires ElementInitializer<I, T, Rank>
          && (sizeof...(Sizes) == Rank)
          && (convertible_to<Sizes, size_t> && ...)
    DArray(I initializer, Sizes... sizes) {
        storeExtents(sizes...);
        data_ = static_cast<T*>(operator new(total_ * sizeof(T), std::align_val_t{alignof(T)}));
        for (size_t linear = 0; linear < total_; linear++) {
            size_t indices[Rank];
            decomposeLinear(linear, indices);
            new (&data_[linear]) T(invokeInit(initializer, indices, make_index_sequence<Rank>{}));
        }
    }

    // Construct with default-initialised elements; T must be default constructible
    template <typename... Sizes>
    requires (sizeof...(Sizes) == Rank)
          && (convertible_to<Sizes, size_t> && ...)
          && requires { T{}; }
    explicit DArray(Sizes... sizes) {
        storeExtents(sizes...);
        data_ = static_cast<T*>(operator new(total_ * sizeof(T), std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < total_; i++)
            new (&data_[i]) T();
    }

    DArray(const DArray& other) : total_(other.total_) {
        for (size_t i = 0; i < Rank; i++) extents_[i] = other.extents_[i];
        if (other.data_) {
            data_ = static_cast<T*>(operator new(total_ * sizeof(T), std::align_val_t{alignof(T)}));
            for (size_t i = 0; i < total_; i++)
                new (&data_[i]) T(other.data_[i]);
        }
    }

    DArray(DArray&& other) noexcept : data_(other.data_), total_(other.total_) {
        for (size_t i = 0; i < Rank; i++) extents_[i] = other.extents_[i];
        other.data_  = nullptr;
        other.total_ = 0;
    }

    DArray& operator=(const DArray& other) {
        if (this != &other) {
            destroy();
            total_ = other.total_;
            for (size_t i = 0; i < Rank; i++) extents_[i] = other.extents_[i];
            if (other.data_) {
                data_ = static_cast<T*>(operator new(total_ * sizeof(T), std::align_val_t{alignof(T)}));
                for (size_t i = 0; i < total_; i++)
                    new (&data_[i]) T(other.data_[i]);
            }
        }
        return *this;
    }

    DArray& operator=(DArray&& other) noexcept {
        if (this != &other) {
            destroy();
            data_  = other.data_;
            total_ = other.total_;
            for (size_t i = 0; i < Rank; i++) extents_[i] = other.extents_[i];
            other.data_  = nullptr;
            other.total_ = 0;
        }
        return *this;
    }

    ~DArray() { destroy(); }

    // Multi-dimensional subscript (C++23); bounds-checked, asserts on out-of-range or null data
    template <typename... Indices>
    requires (sizeof...(Indices) == Rank) && (convertible_to<Indices, size_t> && ...)
    T& operator[](Indices... idxs) {
        assert(data_ != nullptr, "DArray: uninitialized access");
        size_t indices[Rank];
        size_t i = 0;
        ((indices[i++] = static_cast<size_t>(idxs)), ...);
        for (size_t j = 0; j < Rank; j++)
            assert(indices[j] < extents_[j], "DArray: index out of bounds");
        return data_[flatIndex(indices)];
    }

    template <typename... Indices>
    requires (sizeof...(Indices) == Rank) && (convertible_to<Indices, size_t> && ...)
    const T& operator[](Indices... idxs) const {
        assert(data_ != nullptr, "DArray: uninitialized access");
        size_t indices[Rank];
        size_t i = 0;
        ((indices[i++] = static_cast<size_t>(idxs)), ...);
        for (size_t j = 0; j < Rank; j++)
            assert(indices[j] < extents_[j], "DArray: index out of bounds");
        return data_[flatIndex(indices)];
    }

    size_t rank() const { return Rank; }

    size_t extent(size_t index) const {
        assert(index < Rank, "DArray: extent index out of bounds");
        return extents_[index];
    }
};

// -----------------------------------------------------------------------------------------
// SArray — Rank-dimensional array with extents determined at compile time
// -----------------------------------------------------------------------------------------
template <typename T, size_t... Extents>
class SArray {
    static constexpr size_t Rank      = sizeof...(Extents);
    static constexpr size_t TotalSize = (size_t(1) * ... * Extents);
    static constexpr size_t ExtentsArr[] = {Extents...};

    static_assert(Rank > 0, "SArray requires at least one extent");

    // Inline storage — avoids heap allocation and any default construction
    alignas(T) unsigned char raw_[sizeof(T) * TotalSize];

    T*       data()       { return reinterpret_cast<T*>(raw_); }
    const T* data() const { return reinterpret_cast<const T*>(raw_); }

    static size_t flatIndex(const size_t (&indices)[Rank]) {
        size_t flat   = 0;
        size_t stride = 1;
        for (size_t i = Rank; i > 0; i--) {
            flat   += indices[i - 1] * stride;
            stride *= ExtentsArr[i - 1];
        }
        return flat;
    }

    static void decomposeLinear(size_t linear, size_t (&indices)[Rank]) {
        for (size_t i = Rank; i > 0; i--) {
            indices[i - 1]  = linear % ExtentsArr[i - 1];
            linear          /= ExtentsArr[i - 1];
        }
    }

    template <typename I, size_t... Is>
    static T invokeInit(I& init, const size_t (&indices)[Rank], index_sequence<Is...>) {
        return init(indices[Is]...);
    }

public:
    // Construct each element by calling initializer(i0, i1, ..., iRank-1) for its index
    template <typename I>
    requires ElementInitializer<I, T, Rank>
    explicit SArray(I initializer) {
        for (size_t linear = 0; linear < TotalSize; linear++) {
            size_t indices[Rank];
            decomposeLinear(linear, indices);
            new (&data()[linear]) T(invokeInit(initializer, indices, make_index_sequence<Rank>{}));
        }
    }

    // Construct with default-initialised elements; T must be default constructible
    SArray() requires requires { T{}; } {
        for (size_t i = 0; i < TotalSize; i++)
            new (&data()[i]) T();
    }

    SArray(const SArray& other) {
        for (size_t i = 0; i < TotalSize; i++)
            new (&data()[i]) T(other.data()[i]);
    }

    SArray(SArray&& other) {
        for (size_t i = 0; i < TotalSize; i++)
            new (&data()[i]) T(move(other.data()[i]));
    }

    SArray& operator=(const SArray& other) {
        if (this != &other)
            for (size_t i = 0; i < TotalSize; i++)
                data()[i] = other.data()[i];
        return *this;
    }

    SArray& operator=(SArray&& other) {
        if (this != &other)
            for (size_t i = 0; i < TotalSize; i++)
                data()[i] = move(other.data()[i]);
        return *this;
    }

    ~SArray() {
        for (size_t i = 0; i < TotalSize; i++)
            data()[i].~T();
    }

    // Multi-dimensional subscript (C++23); bounds-checked, asserts on out-of-range
    template <typename... Indices>
    requires (sizeof...(Indices) == Rank) && (convertible_to<Indices, size_t> && ...)
    T& operator[](Indices... idxs) {
        size_t indices[Rank];
        size_t i = 0;
        ((indices[i++] = static_cast<size_t>(idxs)), ...);
        for (size_t j = 0; j < Rank; j++)
            assert(indices[j] < ExtentsArr[j], "SArray: index out of bounds");
        return data()[flatIndex(indices)];
    }

    template <typename... Indices>
    requires (sizeof...(Indices) == Rank) && (convertible_to<Indices, size_t> && ...)
    const T& operator[](Indices... idxs) const {
        size_t indices[Rank];
        size_t i = 0;
        ((indices[i++] = static_cast<size_t>(idxs)), ...);
        for (size_t j = 0; j < Rank; j++)
            assert(indices[j] < ExtentsArr[j], "SArray: index out of bounds");
        return data()[flatIndex(indices)];
    }

    size_t rank() const { return Rank; }

    size_t extent(size_t index) const {
        assert(index < Rank, "SArray: extent index out of bounds");
        return ExtentsArr[index];
    }
};

#endif //CROCOS_ARRAY_H