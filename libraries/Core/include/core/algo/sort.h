//
// Created by Spencer Martin on 6/30/25.
//

#ifndef SORT_H
#define SORT_H

#include <core/utility.h>
#include <core/Comparator.h>
#include <core/math.h>

namespace algorithm {
 template <typename T, typename Comparator>
    void insertionSort(T* data, size_t low, size_t high, Comparator& comp) {
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
    template <typename T, typename Comparator>
    void heapify(T* data, size_t heap_size, size_t heap_base, size_t index, Comparator& comp) {
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

    template <typename T, typename Comparator>
    void heapsort(T* data, size_t low, size_t high, Comparator& comp) {
        auto slice_size = static_cast<int64_t>((high - low) + 1);

        //Iteratively go through each non-leaf node from the highest level down to the root and heapify
        for(int64_t i = slice_size / 2 - 1; i >= 0; i--){
            heapify(data, slice_size, low, i, comp);
        }

        for (size_t i = high; i > low; i--) {
            //Swap the root (largest element) with the last element in the heap
            swap(data[low], data[i]);
            //Now exclude this last element from our tree and heapify
            heapify(data, i - low, low, 0, comp);
        }
    }

    template <typename T, typename Comparator>
    size_t medianOfThree(T* data, size_t low, size_t high, Comparator& comp) {
        size_t mid = low + (high - low) / 2;

        if (comp(data[high], data[low])) swap(data[low], data[high]);
        if (comp(data[mid], data[low])) swap(data[mid], data[low]);
        if (comp(data[high], data[mid])) swap(data[high], data[mid]);

        // After sorting: data[low] <= data[mid] <= data[high]
        // Use the median value (data[mid]) as the pivot by swapping it to the front
        swap(data[low], data[mid]);
        return low;
    }

    template <typename T, typename Comparator>
    size_t partitionHoare(T* data, size_t low, size_t high, Comparator& comp) {
        medianOfThree(data, low, high, comp);
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

    template <typename T, typename Comparator>
    void introsort(T* data, size_t low, size_t high, size_t depth_limit, Comparator& comp){
        size_t slice_size = high - low + 1;

        if (slice_size <= INSERTION_SORT_THRESHOLD) {
            insertionSort(data, low, high, comp);
            return;
        }

        if (depth_limit == 0) {
            heapsort(data, low, high, comp);
            return;
        }

        size_t pivotIndex = partitionHoare(data, low, high, comp);

        introsort(data, low, pivotIndex, depth_limit - 1, comp);        // Note: include pivotIndex
        introsort(data, pivotIndex + 1, high, depth_limit - 1, comp);   // Right side
    }

    template <typename T, typename Comparator = DefaultComparator<T>>
    void sort(T* data, const size_t size, Comparator comp = Comparator{}) {
        auto depth_limit = log2floor(size) * 2;
        algorithm::introsort( data, 0, size - 1, depth_limit, comp);
    }

    template <typename T, size_t N, typename Comparator = DefaultComparator<T>>
    void sort(T (&arr)[N], Comparator comp = Comparator{}) {
        auto depth_limit = log2floor(N) * 2;
        algorithm::introsort(&arr[0], 0, N - 1, depth_limit, comp);
    }
}

#endif //SORT_H
