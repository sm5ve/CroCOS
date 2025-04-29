//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_VECTOR_H
#define CROCOS_VECTOR_H

#include "stddef.h"
#include "../Comparator.h"
#include "assert.h"
#include "../math.h"
#include "../utility.h"
#include "../TypeTraits.h"

template <typename T>
class Vector {
private:
    T* data;
    size_t size;
    size_t capacity;
    void reallocate(size_t new_capacity) {
        T* new_data = static_cast<T*>(operator new(sizeof(T) * new_capacity));
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

    template <typename Comparator>
    void insertionSort(size_t low, size_t high, Comparator& comp) {
        for (size_t i = low + 1; i <= high; i++) {
            T key = move(data[i]);
            size_t j = i;
            while (j > low && comp(key, data[j - 1])) {
                data[j] = move(data[j - 1]);
                j--;
            }
            data[j] = move(key);
        }
    }

    //Takes a slice of the data array (from heap_base to heap_base + heap_size) and interprets it as a binary tree where
    //the root lies at heap_base, and the two children of a given node i lie at heap_base + 2i + 1 and heap_base + 2i + 2
    //given the assumption that the binary tree has root A with subtrees T1 and T2 where each Ti satisfies the heap
    //property, it reorders the entire tree to satisfy the heap property
    template <typename Comparator>
    void heapify(size_t heap_size, size_t heap_base, size_t index, Comparator& comp) {
        //start by ensuring the heap property holds at the root of the heap. We begin by assuming the root is the largest
        size_t largest = index;

        while (true) {
            //find the two children for the potential non-heap
            size_t left = 2 * index + 1;
            size_t right = 2 * index + 2;

            //Determine the largest of the three nodes
            //recall comp(A,B) returns whether A < B
            if (left < heap_size && comp(data[largest + heap_base], data[left + heap_base])) {
                largest = left;
            }
            if (right < heap_size && comp(data[largest + heap_base], data[right + heap_base])) {
                largest = right;
            }

            if (largest != index) {
                //If the root is not the largest, swap the largest node to become the root. Now the
                //subtree whose root now contains the old root of the main tree could fail to
                //satisfy the heap property
                swap(data[index + heap_base], data[largest + heap_base]);
                index = largest; // continue heapifying down the tree
            } else {
                break; // heap property is satisfied
            }
        }
    }

    template <typename Comparator>
    void heapsort(size_t low, size_t high, Comparator& comp) {
        auto slice_size = static_cast<int64_t>((high - low) + 1);

        //Iteratively go through each non-leaf node from the highest level down to the root and heapify
        for(int64_t i = slice_size / 2 - 1; i >= 0; i--){
            heapify(slice_size, low, i, comp);
        }

        for (size_t i = high; i > low; i--) {
            //Swap the root (largest element) with the last element in the heap
            swap(data[low], data[i]);
            //Now exclude this last element from our tree and heapify
            heapify(i - low, low, 0, comp);
        }
    }

    template <typename Comparator>
    size_t medianOfThree(size_t low, size_t high, Comparator& comp) {
        size_t mid = low + (high - low) / 2;

        if (comp(data[high], data[low])) swap(data[low], data[high]);
        if (comp(data[mid], data[low])) swap(data[mid], data[low]);
        if (comp(data[high], data[mid])) swap(data[high], data[mid]);

        // After sorting: data[low] <= data[mid] <= data[high]
        // Use the median value (data[mid]) as the pivot by swapping it to the front
        swap(data[low], data[mid]);
        return low;
    }

    template <typename Comparator>
    size_t partitionHoare(size_t low, size_t high, Comparator& comp) {
        medianOfThree(low, high, comp);
        T& pivot = data[low]; // Note: pivot is chosen as the first element
        size_t i = low - 1;
        size_t j = high + 1;

        while (true) {
            do {
                i++;
            } while (comp(data[i], pivot));  // Move right until data[i] >= pivot

            do {
                j--;
            } while (comp(pivot, data[j]));  // Move left until data[j] <= pivot

            if (i >= j)
                return j;  // return the last element of the left partition

            swap(data[i], data[j]);
        }
    }

#define INSERTION_SORT_THRESHOLD 16

    template <typename Comparator>
    void introsort(size_t low, size_t high, size_t depth_limit, Comparator& comp){
        size_t slice_size = high - low + 1;

        if (slice_size <= INSERTION_SORT_THRESHOLD) {
            insertionSort(low, high, comp);
            return;
        }

        if (depth_limit == 0) {
            heapsort(low, high, comp);
            return;
        }

        size_t pivotIndex = partitionHoare(low, high, comp);

        introsort(low, pivotIndex, depth_limit - 1, comp);        // Note: include pivotIndex
        introsort(pivotIndex + 1, high, depth_limit - 1, comp);   // Right side
    }
public:
    //Default constructor
    Vector() : data(nullptr), size(0), capacity(0) {}

    //Constructor with initial capacity
    Vector(size_t init_capacity) : size(0), capacity(init_capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * init_capacity));
    }

    //Constructor with initial data provided.
    Vector(T* array, size_t input_size) : size(input_size), capacity(input_size) {
        data = static_cast<T*>(operator new(sizeof(T) * size));
        for (size_t i = 0; i < size; i++) {
            data[i] = array[i];
        }
    }

    //Copy constructor
    Vector(const Vector& other) : size(other.size), capacity(other.capacity) {
        data = static_cast<T*>(operator new(sizeof(T) * other.capacity));
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
        data = static_cast<T*>(operator new(sizeof(T) * capacity));
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

    size_t getCapacity() const {
        return capacity;
    }

    T& operator[](size_t index) {
        assert(index < size, "Index out of bounds");
        return data[index];
    }

    const T& operator[](size_t index) const {
        assert(index < size, "Index out of bounds");
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

    void ensureRoom(size_t openSlots){
        size_t min_size = size + openSlots;
        if(min_size < capacity){
            reallocate(min_size + 4); //just add a little wiggle room in case?
        }
    }

    template <typename Comparator = DefaultComparator<T>>
    void sort(Comparator comp = Comparator{}){
        auto depth_limit = log2floor(size) * 2;
        introsort( 0, size - 1, depth_limit, comp);
    }
};

#endif //CROCOS_VECTOR_H
