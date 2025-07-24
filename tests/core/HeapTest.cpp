//
// Unit tests for Core Heap class
// Created by Spencer Martin on 7/24/25.
//

#include "../harness/TestHarness.h"
#include <core/ds/Heap.h>

using namespace CroCOSTest;

TEST(HeapDefaultConstructor) {
    MaxHeap<int> heap;
    ASSERT_EQ(0u, heap.size());
    ASSERT_TRUE(heap.empty());
}

TEST(HeapPushAndTop) {
    MaxHeap<int> heap;
    heap.push(10);
    heap.push(20);
    heap.push(5);
    heap.push(15);
    
    ASSERT_EQ(4u, heap.size());
    ASSERT_FALSE(heap.empty());
    ASSERT_EQ(20, heap.top());  // Max element should be at top
}

TEST(HeapPop) {
    MaxHeap<int> heap;
    heap.push(10);
    heap.push(20);
    heap.push(5);
    heap.push(15);
    heap.push(25);
    
    // Should pop in descending order (max heap)
    ASSERT_EQ(25, heap.pop());
    ASSERT_EQ(20, heap.pop());
    ASSERT_EQ(15, heap.pop());
    ASSERT_EQ(10, heap.pop());
    ASSERT_EQ(5, heap.pop());
    
    ASSERT_TRUE(heap.empty());
}

TEST(MinHeapOrdering) {
    MinHeap<int> heap;
    heap.push(10);
    heap.push(20);
    heap.push(5);
    heap.push(15);
    heap.push(1);
    
    // Should pop in ascending order (min heap)
    ASSERT_EQ(1, heap.pop());
    ASSERT_EQ(5, heap.pop());
    ASSERT_EQ(10, heap.pop());
    ASSERT_EQ(15, heap.pop());
    ASSERT_EQ(20, heap.pop());
    
    ASSERT_TRUE(heap.empty());
}

TEST(HeapFromVector) {
    Vector<int> data;
    data.push(30);
    data.push(10);
    data.push(40);
    data.push(20);
    
    MaxHeap<int> heap(data);
    ASSERT_EQ(4u, heap.size());
    ASSERT_EQ(40, heap.top());  // Max should be at top after heapify
    
    // Verify all elements are in correct heap order
    ASSERT_EQ(40, heap.pop());
    ASSERT_EQ(30, heap.pop());
    ASSERT_EQ(20, heap.pop());
    ASSERT_EQ(10, heap.pop());
}

TEST(HeapMaintainsProperty) {
    MaxHeap<int> heap;
    
    // Add elements in random order
    int elements[] = {15, 3, 22, 8, 11, 6, 19, 1, 25, 4};
    for (int elem : elements) {
        heap.push(elem);
        // After each push, the max element should be accessible
        // We can't verify the complete heap property without internal access,
        // but we can verify the max is always at the top
        ASSERT_GE(heap.top(), elem);
    }
    
    // Pop all elements - should come out in sorted order
    int prev = heap.pop();
    while (!heap.empty()) {
        int current = heap.pop();
        ASSERT_LE(current, prev);  // Each element should be <= previous
        prev = current;
    }
}

TEST(HeapCapacityOperations) {
    MaxHeap<int> heap;
    size_t initialCapacity = heap.capacity();
    
    // Add enough elements to trigger capacity growth
    for (int i = 0; i < 20; i++) {
        heap.push(i);
        ASSERT_GE(heap.capacity(), heap.size());
    }
    
    ASSERT_GT(heap.capacity(), initialCapacity);
    
    // Test reserve
    heap.reserve(100);
    ASSERT_GE(heap.capacity(), 100u);
    
    // Test clear
    heap.clear();
    ASSERT_EQ(0u, heap.size());
    ASSERT_TRUE(heap.empty());
}