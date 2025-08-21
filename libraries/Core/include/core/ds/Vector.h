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

template <typename T>
class Vector {
private:
    T* data;
    size_t size;
    size_t capacity;
    void reallocate(size_t new_capacity) {
        T* new_data = static_cast<T*>(operator new(sizeof(T) * new_capacity, std::align_val_t{alignof(T)}));
        // Copy existing elements to the new buffer
        for (size_t i = 0; i < size; ++i) {
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
        if (size == capacity) {
            // Double the capacity when the vector is full
            reallocate(capacity * 2);
        } else if (size <= capacity / 4 && capacity > 8) {
            // Shrink the buffer when size drops below 1/4th of capacity
            // Ensure we don't shrink too much (e.g., to a 1-element buffer)
            reallocate(capacity / 2);
        }
    }
public:
    //Default constructor
    Vector() : data(nullptr), size(0), capacity(0) {}

    //Constructor with initial capacity
    Vector(size_t init_capacity) : size(0), capacity(init_capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * init_capacity, std::align_val_t{alignof(T)}));
    }

    //Constructor with initial data provided.
    Vector(T* array, size_t input_size) : size(input_size), capacity(input_size) {
        data = static_cast<T*>(operator new(sizeof(T) * size, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < size; i++) {
            data[i] = array[i];
        }
    }

    //Copy constructor
    Vector(const Vector& other) : size(other.size), capacity(other.capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * other.capacity, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < size; i++) {
            data[i] = other.data[i];
        }
    }

    //Copy assignment
    Vector& operator=(const Vector& other) {
        if (this == &other) return *this;
        for(size_t i = 0; i < size; i++){
            data[i].~T();
        }
        operator delete(data);
        size = other.size;
        capacity = other.capacity;
        data = static_cast<T*>(operator new(sizeof(T) * capacity, std::align_val_t{alignof(T)}));
        for (size_t i = 0; i < size; i++) {
            data[i] = other.data[i];
        }
        return *this;
    }

    //Move constructor
    Vector(Vector&& other) noexcept :
    data(other.data), size(other.size), capacity(other.capacity) {
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
    }

    //Move assignment
    Vector& operator=(Vector&& other) noexcept {
        if (this == &other) return *this;
        for(size_t i = 0; i < size; i++){
            data[i].~T();
        }
        operator delete(data);
        data = other.data;
        size = other.size;
        capacity = other.capacity;
        other.data = nullptr;
        other.size = 0;
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
        for (size_t i = 0; i < size; ++i) {
            data[i].~T();  //Remember to call the destructors for each element in our buffer
        }
        operator delete(data);
    }

    void push(const T& value) {
        reallocate_if_necessary();
        if constexpr (is_trivially_copyable_v<T>) {
            data[size] = value;
        }
        else{
            new (&data[size]) T(value);  // Placement new for the new element
        }
        ++size;
    }

    void push(T&& value) {
        reallocate_if_necessary();
        if constexpr (is_trivially_copyable_v<T>) {
            data[size] = value;
        }
        else{
            new (&data[size]) T(move(value));  // Placement new for the new element
        }
        ++size;
    }

    void remove(size_t index) {
        assert(index < size, "Index out of bounds");
        data[index].~T();  // Explicitly call the destructor
        // Move elements to fill the gap
        for (size_t i = index; i < size - 1; ++i) {
            if constexpr (is_trivially_copyable_v<T>) {
                data[i] = data[i + 1];
            }
            else{
                data[i] = move(data[i + 1]);
            }
        }
        --size;
        reallocate_if_necessary();  // Shrink the buffer if necessary
    }

    T pop() {
        assert(size > 0, "Cannot pop from empty vector");
        --size;
        T result = move(data[size]);  // Move the last element
        data[size].~T();  // Explicitly call the destructor
        reallocate_if_necessary();  // Shrink the buffer if necessary
        return result;
    }

    void insert(size_t index, const T& value) {
        assert(index <= size, "Index out of bounds");
        reallocate_if_necessary();
        if(index == size){
            push(value);
            return;
        }
        // Move elements to make room for the new element
        for (size_t i = size; i > index; --i) {
            if constexpr (is_trivially_copyable_v<T>) {
                data[i] = data[i - 1];
            }
            else{
                data[i] = move(data[i - 1]);
            }
        }
        new (data + index) T(value);  // Placement new for the new element
        ++size;
    }

    void insert(size_t index, T&& value) {
        assert(index <= size, "Index out of bounds");
        reallocate_if_necessary();
        if(index == size){
            push(value);
            return;
        }
        // Move elements to make room for the new element
        for (size_t i = size; i > index; --i) {
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
        ++size;
    }

    size_t getSize() const {
        return size;
    }

    bool empty() const {
        return size == 0;
    }

    size_t getCapacity() const {
        return capacity;
    }

    template <UnsignedIntegral Index>
    T& operator[](Index index) {
        assert(index < size, "Index out of bounds");
        return data[index];
    }

    template <UnsignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < size, "Index out of bounds");
        return data[index];
    }

    template <SignedIntegral Index>
    T& operator[](Index index) {
        assert(index < (Index)size, "Index out of bounds");
        assert((Index)size + index >= 0, "Index out of bounds ", index, " has size ", size);
        if (index < 0)
            return data[size + index];
        return data[index];
    }

    template <SignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < (Index)size, "Index out of bounds");
        assert((Index)size + index >= 0, "Index out of bounds ", index, " has size ", size);
        if (index < 0)
            return data[size + index];
        return data[index];
    }

    T* begin() {
        return data;
    }

    const T* begin() const {
        return data;
    }

    T* end() {
        return data + size;
    }

    const T* end() const {
        return data + size;
    }

    T* top() {
        return &data[size - 1];
    }

    void ensureRoom(size_t openSlots){
        size_t min_size = size + openSlots;
        if(min_size > capacity){
            reallocate(min_size + 4); //just add a little wiggle room in case?
        }
    }

    template <typename Comparator = DefaultComparator<T>>
    void sort(Comparator comp = Comparator{}){
        algorithm::sort(data, size, comp);
    }
};

#endif //CROCOS_VECTOR_H
