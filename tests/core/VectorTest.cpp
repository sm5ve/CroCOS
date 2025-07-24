//
// Unit tests for Core Vector class
// Created by Spencer Martin on 7/24/25.
//

#include "../harness/TestHarness.h"
#include <core/ds/Vector.h>

using namespace CroCOSTest;

TEST(VectorDefaultConstructor) {
    Vector<int> vec;
    ASSERT_EQ(0u, vec.getSize());
    ASSERT_EQ(0u, vec.getCapacity());
}

TEST(VectorPushAndSize) {
    Vector<int> vec;
    vec.push(42);
    vec.push(24);
    vec.push(13);
    
    ASSERT_EQ(3u, vec.getSize());
    ASSERT_EQ(42, vec[0]);
    ASSERT_EQ(24, vec[1]);
    ASSERT_EQ(13, vec[2]);
}

TEST(VectorCapacityGrowth) {
    Vector<int> vec;
    
    // Initially empty
    ASSERT_EQ(0u, vec.getCapacity());
    
    // First push should allocate initial capacity
    vec.push(1);
    ASSERT_GT(vec.getCapacity(), 0u);
    size_t firstCapacity = vec.getCapacity();
    
    // Fill up to capacity
    while (vec.getSize() < firstCapacity) {
        vec.push(static_cast<int>(vec.getSize()));
    }
    
    // Next push should double capacity
    vec.push(999);
    ASSERT_GT(vec.getCapacity(), firstCapacity);
}

TEST(VectorPop) {
    Vector<int> vec;
    vec.push(10);
    vec.push(20);
    vec.push(30);
    
    ASSERT_EQ(30, vec.pop());
    ASSERT_EQ(2u, vec.getSize());
    
    ASSERT_EQ(20, vec.pop());
    ASSERT_EQ(1u, vec.getSize());
    
    ASSERT_EQ(10, vec.pop());
    ASSERT_EQ(0u, vec.getSize());
}

TEST(VectorInsert) {
    Vector<int> vec;
    vec.push(1);
    vec.push(3);
    vec.push(5);
    
    // Insert at beginning
    vec.insert(0, 0);
    ASSERT_EQ(4u, vec.getSize());
    ASSERT_EQ(0, vec[0]);
    ASSERT_EQ(1, vec[1]);
    
    // Insert in middle
    vec.insert(2, 2);
    ASSERT_EQ(5u, vec.getSize());
    ASSERT_EQ(0, vec[0]);
    ASSERT_EQ(1, vec[1]);
    ASSERT_EQ(2, vec[2]);
    ASSERT_EQ(3, vec[3]);
    ASSERT_EQ(5, vec[4]);
}

TEST(VectorRemove) {
    Vector<int> vec;
    vec.push(10);
    vec.push(20);
    vec.push(30);
    vec.push(40);
    
    // Remove from middle
    vec.remove(1);
    ASSERT_EQ(3u, vec.getSize());
    ASSERT_EQ(10, vec[0]);
    ASSERT_EQ(30, vec[1]);
    ASSERT_EQ(40, vec[2]);
    
    // Remove from beginning
    vec.remove(0);
    ASSERT_EQ(2u, vec.getSize());
    ASSERT_EQ(30, vec[0]);
    ASSERT_EQ(40, vec[1]);
}

TEST(VectorCopyConstructor) {
    Vector<int> original;
    original.push(1);
    original.push(2);
    original.push(3);
    
    Vector<int> copy(original);
    ASSERT_EQ(original.getSize(), copy.getSize());
    
    for (size_t i = 0; i < original.getSize(); i++) {
        ASSERT_EQ(original[i], copy[i]);
    }
    
    // Verify independence
    copy.push(4);
    ASSERT_NE(original.getSize(), copy.getSize());
}

TEST(VectorMoveConstructor) {
    Vector<int> original;
    original.push(1);
    original.push(2);
    original.push(3);
    size_t originalSize = original.getSize();
    
    Vector<int> moved(static_cast<Vector<int>&&>(original));
    ASSERT_EQ(originalSize, moved.getSize());
    ASSERT_EQ(0u, original.getSize());  // Original should be empty after move
    
    ASSERT_EQ(1, moved[0]);
    ASSERT_EQ(2, moved[1]);
    ASSERT_EQ(3, moved[2]);
}

// Tests to verify that Vector's internal asserts are caught by our test framework
TEST(VectorOutOfBoundsAccessThrows) {
    Vector<int> vec;
    vec.push(42);
    vec.push(24);
    
    bool exceptionCaught = false;
    try {
        // This should trigger assert(index < size, "Index out of bounds") in Vector.h:148
        int value = vec[5];  // Out of bounds access
        (void)value; // Avoid unused variable warning
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("Index out of bounds") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

TEST(VectorPopEmptyThrows) {
    Vector<int> vec;  // Empty vector
    
    bool exceptionCaught = false;
    try {
        // This should trigger assert(size > 0, "Cannot pop from empty vector") in Vector.h:164
        vec.pop();
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("Cannot pop from empty vector") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

TEST(VectorInsertInvalidIndexThrows) {
    Vector<int> vec;
    vec.push(1);
    vec.push(2);
    
    bool exceptionCaught = false;
    try {
        // This should trigger assert(index <= size, "Index out of bounds") in Vector.h:173
        vec.insert(5, 99);  // Index 5 is out of bounds for size 2
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("Index out of bounds") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}

TEST(VectorRemoveInvalidIndexThrows) {
    Vector<int> vec;
    vec.push(1);
    vec.push(2);
    
    bool exceptionCaught = false;
    try {
        // This should trigger assert(index <= size, "Index out of bounds") in Vector.h:193
        vec.remove(5);  // Index 5 is out of bounds for size 2  
    } catch (const AssertionFailure& e) {
        exceptionCaught = true;
        std::string message = e.what();
        ASSERT_TRUE(message.find("Index out of bounds") != std::string::npos);
    }
    ASSERT_TRUE(exceptionCaught);
}