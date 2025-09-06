//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_VECTOR_H
#define CROCOS_VECTOR_H

#include "stddef.h"
#include "core/Comparator.h"
#include "assert.h"
#include "core/math.h"
#include "core/utility.h"
#include "core/TypeTraits.h"
#include <core/algo/sort.h>
#include <initializer_list.h>
#include <core/Iterator.h>

template <typename T>
class Vector {
private:
    T* data;
    size_t _size;
    size_t capacity;
    void reallocate(size_t new_capacity) {
        T* new_data = static_cast<T*>(operator new(sizeof(T) * new_capacity, std::align_val_t{alignof(T)}));
        // Copy existing elements to the new buffer
        for (size_t i = 0; i < _size; ++i) {
            if constexpr (is_trivially_copyable_v<T>) {
                new_data[i] = data[i];
            }
            else{
                new(&new_data[i]) T(move(data[i]));  // Move each element into the new buffer
                data[i].~T();  // Explicitly call the destructor of old element
            }
        }
        operator delete(data);  // Deallocate old buffer
        data = new_data;  // Point to the new buffer
        capacity = new_capacity;  // Update capacity
    }

    void reallocate_if_necessary() {
        if (capacity == 0) {
            reallocate(8);  // Start with a reasonable default capacity
        }
        if (_size == capacity) {
            // Double the capacity when the vector is full
            reallocate(capacity * 2);
        } else if (_size <= capacity / 4 && capacity > 8) {
            // Shrink the buffer when size drops below 1/4th of capacity
            // Ensure we don't shrink too much (e.g., to a 1-element buffer)
            reallocate(capacity / 2);
        }
    }
public:
    //Default constructor
    Vector() : data(nullptr), _size(0), capacity(0) {}

    //Constructor with initial capacity
    Vector(size_t init_capacity) : _size(0), capacity(init_capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * init_capacity, std::align_val_t{alignof(T)}));
    }

    //Constructor with initial data provided.
    Vector(T* array, size_t input_size) : _size(input_size), capacity(input_size) {
        data = static_cast<T*>(operator new(sizeof(T) * _size, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < _size; i++) {
            data[i] = array[i];
        }
    }

    //Copy constructor
    Vector(const Vector& other) : _size(other._size), capacity(other.capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * other.capacity, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < _size; i++) {
            data[i] = other.data[i];
        }
    }

    template <typename Itr>
    requires IterableWithValueType<Itr, T>
    explicit Vector(Itr itr) : Vector(5){
        for (auto i : itr) {
            push(i);
        }
    }


    //Copy assignment
    Vector& operator=(const Vector& other) {
        if (this == &other) return *this;
        for(size_t i = 0; i < _size; i++){
            data[i].~T();
        }
        operator delete(data);
        _size = other._size;
        capacity = other.capacity;
        data = static_cast<T*>(operator new(sizeof(T) * capacity, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < _size; i++) {
            data[i] = other.data[i];
        }
        return *this;
    }

    //Move constructor
    Vector(Vector&& other) noexcept :
    data(other.data), _size(other._size), capacity(other.capacity) {
        other.data = nullptr;
        other._size = 0;
        other.capacity = 0;
    }

    //Move assignment
    Vector& operator=(Vector&& other) noexcept {
        if (this == &other) return *this;
        for(size_t i = 0; i < _size; i++){
            data[i].~T();
        }
        operator delete(data);
        data = other.data;
        _size = other._size;
        capacity = other.capacity;
        other.data = nullptr;
        other._size = 0;
        other.capacity = 0;
        return *this;
    }

    Vector(std::initializer_list<T> initializer) : Vector(initializer.size()){
        for (auto& element : initializer) {
            this -> push(element);
        }
    }

    //Destructor
    ~Vector() {
        for (size_t i = 0; i < _size; ++i) {
            data[i].~T();  //Remember to call the destructors for each element in our buffer
        }
        operator delete(data);
    }

    void push(const T& value) {
        reallocate_if_necessary();
        if constexpr (is_trivially_copyable_v<T>) {
            data[_size] = value;
        }
        else{
            new (&data[_size]) T(value);  // Placement new for the new element
        }
        ++_size;
    }

    void push(T&& value) {
        reallocate_if_necessary();
        if constexpr (is_trivially_copyable_v<T>) {
            data[_size] = value;
        }
        else{
            new (&data[_size]) T(move(value));  // Placement new for the new element
        }
        ++_size;
    }

    void remove(size_t index) {
        assert(index < _size, "Index out of bounds");
        data[index].~T();  // Explicitly call the destructor
        // Move elements to fill the gap
        for (size_t i = index; i < _size - 1; ++i) {
            if constexpr (is_trivially_copyable_v<T>) {
                data[i] = data[i + 1];
            }
            else{
                data[i] = move(data[i + 1]);
            }
        }
        --_size;
        reallocate_if_necessary();  // Shrink the buffer if necessary
    }

    T pop() {
        assert(_size > 0, "Cannot pop from empty vector");
        --_size;
        T result = move(data[_size]);  // Move the last element
        data[_size].~T();  // Explicitly call the destructor
        reallocate_if_necessary();  // Shrink the buffer if necessary
        return result;
    }

    void insert(size_t index, const T& value) {
        assert(index <= _size, "Index out of bounds");
        reallocate_if_necessary();
        if(index == _size){
            push(value);
            return;
        }
        // Move elements to make room for the new element
        for (size_t i = _size; i > index; --i) {
            if constexpr (is_trivially_copyable_v<T>) {
                data[i] = data[i - 1];
            }
            else{
                data[i] = move(data[i - 1]);
            }
        }
        new (data + index) T(value);  // Placement new for the new element
        ++_size;
    }

    void insert(size_t index, T&& value) {
        assert(index <= _size, "Index out of bounds");
        reallocate_if_necessary();
        if(index == _size){
            push(value);
            return;
        }
        // Move elements to make room for the new element
        for (size_t i = _size; i > index; --i) {
            if constexpr (is_trivially_copyable_v<T>) {
                data[i] = data[i - 1];
            }
            else{
                data[i] = move(data[i - 1]);
            }
        }

        if constexpr (is_trivially_copyable_v<T>) {
            data[index] = value;
        }
        else{
            new (data + index) T(move(value));  // Placement new for the new element
        }
        ++_size;
    }

    size_t size() const {
        return _size;
    }

    bool empty() const {
        return _size == 0;
    }

    size_t getCapacity() const {
        return capacity;
    }

    template <UnsignedIntegral Index>
    T& operator[](Index index) {
        assert(index < _size, "Index out of bounds");
        return data[index];
    }

    template <UnsignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < _size, "Index out of bounds");
        return data[index];
    }

    template <SignedIntegral Index>
    T& operator[](Index index) {
        assert(index < (Index)_size, "Index out of bounds");
        assert((Index)_size + index >= 0, "Index out of bounds ", index, " has size ", _size);
        if (index < 0)
            return data[_size + index];
        return data[index];
    }

    template <SignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < (Index)_size, "Index out of bounds");
        assert((Index)_size + index >= 0, "Index out of bounds ", index, " has size ", _size);
        if (index < 0)
            return data[_size + index];
        return data[index];
    }

    T* begin() {
        return data;
    }

    const T* begin() const {
        return data;
    }

    T* end() {
        return data + _size;
    }

    const T* end() const {
        return data + _size;
    }

    T* top() {
        return &data[_size - 1];
    }

    void ensureRoom(size_t openSlots){
        size_t min_size = _size + openSlots;
        if(min_size > capacity){
            reallocate(min_size + 4); //just add a little wiggle room in case?
        }
    }

    void shrinkToFit() {
        reallocate(_size);
    }

    template <typename Comparator = DefaultComparator<T>>
    void sort(Comparator comp = Comparator{}){
        algorithm::sort(data, _size, comp);
    }

    template <typename Comparator = DefaultComparator<T>>
    void mergeIn(const T& value, Comparator comp = Comparator{}) {
        push(value);
        if (_size == 1) return;
        size_t i = _size - 1;
        //While data[i] < data[i - 1]
        while (i > 0 && comp(data[i], data[i - 1])) {
            swap(data[i], data[i - 1]);
            --i;
        }
    }

    class ReverseIterator {
        T* ptr;
        friend class Vector;
        explicit ReverseIterator(T* p) : ptr(p) {}
    public:
        T& operator*() const { return *ptr; }
        ReverseIterator& operator++() {
            --ptr;
            return *this;
        }
        ReverseIterator operator++(int) {
            --ptr;
            return *this;
        }
        bool operator==(const ReverseIterator& other) const {
            return ptr == other.ptr;
        }
    };

    IteratorRange<ReverseIterator> reverse() const {
        return IteratorRange<ReverseIterator>(ReverseIterator(data + _size - 1), ReverseIterator(data - 1));
    }

    void clear() {
        for (size_t i = 0; i < _size; ++i) {
            data[i].~T();  //Remember to call the destructors for each element in our buffer
        }
        operator delete(data);
        _size = 0;
        capacity = 0;
    }
};

#endif //CROCOS_VECTOR_H
