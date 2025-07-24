//
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_HEAP_H
#define CROCOS_HEAP_H

#include "Vector.h"
#include <core/Comparator.h>
#include <core/algo/sort.h>

template <typename T, typename Comp = DefaultComparator<T>>
class Heap {
private:
    Vector<T> data;
    Comp comparator;

    // Helper function to get parent index
    size_t parent(size_t index) const {
        return (index - 1) / 2;
    }

    // Helper function to get left child index
    size_t leftChild(size_t index) const {
        return 2 * index + 1;
    }

    // Helper function to get right child index
    size_t rightChild(size_t index) const {
        return 2 * index + 2;
    }

    // Bubble up element at index to maintain heap property
    void bubbleUp(size_t index) {
        while (index > 0) {
            size_t parentIndex = parent(index);
            // If parent is already larger (or equal), heap property is satisfied
            if (!comparator(data[parentIndex], data[index])) {
                break;
            }
            // Swap with parent and continue up
            swap(data[index], data[parentIndex]);
            index = parentIndex;
        }
    }

    void bubbleDown(size_t index) {
        algorithm::heapify(data.begin(), data.getSize(), 0, index, comparator);
    }

public:
    // Default constructor
    Heap(Comp comp = Comp{}) : comparator(comp) {}

    // Constructor with initial capacity
    explicit Heap(size_t initial_capacity, Comp comp = Comp{}) 
        : data(initial_capacity), comparator(comp) {}

    // Constructor from existing data (heapifies the data)
    Heap(const Vector<T>& input_data, Comp comp = Comp{}) 
        : data(input_data), comparator(comp) {
        buildHeap();
    }

    // Constructor from raw array (heapifies the data)
    Heap(T* array, size_t size, Comp comp = Comp{}) 
        : data(array, size), comparator(comp) {
        buildHeap();
    }

    // Build heap from current data (heapify all elements)
    void buildHeap() {
        if (data.getSize() <= 1) return;
        
        // Start from the last non-leaf node and heapify down
        for (int64_t i = static_cast<int64_t>(data.getSize()) / 2 - 1; i >= 0; i--) {
            bubbleDown(static_cast<size_t>(i));
        }
    }

    // Insert element into heap
    void push(const T& value) {
        data.push(value);
        bubbleUp(data.getSize() - 1);
    }

    void push(T&& value) {
        data.push(move(value));
        bubbleUp(data.getSize() - 1);
    }

    // Extract maximum element (root)
    T pop() {
        assert(data.getSize() > 0, "Cannot pop from empty heap");
        
        T result = move(data[0]);
        
        if (data.getSize() == 1) {
            data.pop();
        } else {
            // Move last element to root and restore heap property
            data[0] = move(data[data.getSize() - 1]);
            data.pop();
            bubbleDown(0);
        }
        
        return result;
    }

    // Peek at maximum element without removing it
    const T& top() const {
        assert(data.getSize() > 0, "Cannot peek at empty heap");
        return data[0];
    }

    T& top() {
        assert(data.getSize() > 0, "Cannot peek at empty heap");
        return data[0];
    }

    // Get size of heap
    size_t size() const {
        return data.getSize();
    }

    // Check if heap is empty
    bool empty() const {
        return data.getSize() == 0;
    }

    // Get capacity of underlying storage
    size_t capacity() const {
        return data.getCapacity();
    }

    // Clear all elements
    void clear() {
        data = Vector<T>();
    }

    // Reserve capacity in underlying storage
    void reserve(size_t new_capacity) {
        data.ensureRoom(new_capacity - data.getSize());
    }

    // Iterator support (note: iteration order is not sorted)
    T* begin() {
        return data.begin();
    }

    const T* begin() const {
        return data.begin();
    }

    T* end() {
        return data.end();
    }

    const T* end() const {
        return data.end();
    }
};

// Convenience type aliases
template <typename T>
using MaxHeap = Heap<T, DefaultComparator<T>>;

template <typename T>
using MinHeap = Heap<T, ReversedDefaultComparator<T>>;

#endif //CROCOS_HEAP_H