#include "../test.h"
#include <harness/TestHarness.h>
#include <core/ds/LinkedList.h>

TEST(LinkedListBasicOperations) {
    LinkedList<int> list;
    
    auto node1 = list.pushBack(1);
    auto node2 = list.pushBack(2);
    auto node3 = list.pushFront(0);
    
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    ASSERT_NE(node3, nullptr);
    
    int values[3];
    int i = 0;
    for (auto& val : list.forward()) {
        values[i++] = val;
    }
    ASSERT_EQ(values[0], 0);
    ASSERT_EQ(values[1], 1);
    ASSERT_EQ(values[2], 2);
}

TEST(LinkedListPopOperations) {
    LinkedList<int> list;
    
    auto front = list.popFront();
    auto back = list.popBack();
    ASSERT_FALSE(front.occupied());
    ASSERT_FALSE(back.occupied());
    
    list.pushBack(10);
    list.pushBack(20);
    list.pushBack(30);
    
    auto frontVal = list.popFront();
    auto backVal = list.popBack();
    
    ASSERT_TRUE(frontVal.occupied());
    ASSERT_TRUE(backVal.occupied());
    ASSERT_EQ(*frontVal, 10);
    ASSERT_EQ(*backVal, 30);
    
    auto remaining = list.popFront();
    ASSERT_TRUE(remaining.occupied());
    ASSERT_EQ(*remaining, 20);
    
    auto empty = list.popFront();
    ASSERT_FALSE(empty.occupied());
}

TEST(LinkedListRemoveOperations) {
    LinkedList<int> list;
    
    auto node1 = list.pushBack(1);
    auto node2 = list.pushBack(2);
    auto node3 = list.pushBack(3);
    
    list.remove(node2);
    
    int values[2];
    int i = 0;
    for (auto& val : list.forward()) {
        values[i++] = val;
    }
    ASSERT_EQ(i, 2);
    ASSERT_EQ(values[0], 1);
    ASSERT_EQ(values[1], 3);
    
    ASSERT_EQ(node2, nullptr);
}

TEST(LinkedListInitializerListConstructor) {
    LinkedList<int> list{1, 2, 3, 4, 5};
    
    int expected[] = {1, 2, 3, 4, 5};
    int i = 0;
    for (auto& val : list.forward()) {
        ASSERT_EQ(val, expected[i++]);
    }
    ASSERT_EQ(i, 5);
}

TEST(LinkedListBufferConstructor) {
    int buffer[] = {10, 20, 30};
    LinkedList<int> list(buffer, 3);
    
    int i = 0;
    for (auto& val : list.forward()) {
        ASSERT_EQ(val, buffer[i++]);
    }
    ASSERT_EQ(i, 3);
}

TEST(LinkedListBidirectionalIteration) {
    LinkedList<int> list{1, 2, 3};
    
    int forwardVals[3];
    int backwardVals[3];
    
    int i = 0;
    for (auto& val : list.forward()) {
        forwardVals[i++] = val;
    }
    
    i = 0;
    for (auto& val : list.backward()) {
        backwardVals[i++] = val;
    }
    
    ASSERT_EQ(forwardVals[0], 1);
    ASSERT_EQ(forwardVals[1], 2);
    ASSERT_EQ(forwardVals[2], 3);
    
    ASSERT_EQ(backwardVals[0], 3);
    ASSERT_EQ(backwardVals[1], 2);
    ASSERT_EQ(backwardVals[2], 1);
}

TEST(LinkedListSingleElementOperations) {
    LinkedList<int> list;
    
    auto node = list.pushBack(42);
    ASSERT_NE(node, nullptr);
    
    auto val = list.popFront();
    ASSERT_TRUE(val.occupied());
    ASSERT_EQ(*val, 42);
    
    auto empty = list.popBack();
    ASSERT_FALSE(empty.occupied());
}

TEST(LinkedListEmptyIterations) {
    LinkedList<int> list;
    
    int count = 0;
    for (auto& val : list.forward()) {
        count++;
    }
    ASSERT_EQ(count, 0);
    
    count = 0;
    for (auto& val : list.backward()) {
        count++;
    }
    ASSERT_EQ(count, 0);
}

TEST(LinkedListRemoveDuringIteration) {
    LinkedList<int> list{1, 2, 3, 4, 5};
    
    // Remove all even numbers during iteration
    for (auto& node : list.forwardNodes()) {
        if (node.data % 2 == 0) {
            auto nodePtr = &node;
            list.remove(nodePtr);
        }
    }
    
    // Should have only odd numbers left: 1, 3, 5
    int expectedOdds[] = {1, 3, 5};
    int i = 0;
    for (auto& val : list.forward()) {
        ASSERT_EQ(val, expectedOdds[i++]);
    }
    ASSERT_EQ(i, 3);
}

TEST(LinkedListRemoveAllDuringIteration) {
    LinkedList<int> list{10, 20, 30};
    
    // Remove all nodes during iteration
    for (auto& node : list.forwardNodes()) {
        auto nodePtr = &node;
        list.remove(nodePtr);
    }
    
    // List should be empty
    int count = 0;
    for (auto& val : list.forward()) {
        count++;
    }
    ASSERT_EQ(count, 0);
    
    ASSERT_EQ(list.head(), nullptr);
    ASSERT_EQ(list.tail(), nullptr);
}

TEST(LinkedListRemoveHeadAndTailDuringIteration) {
    LinkedList<int> list{1, 2, 3, 4, 5};
    
    // Remove first and last elements during iteration
    int nodeCount = 0;
    
    // Count nodes first
    for (auto& node : list.forwardNodes()) {
        nodeCount++;
    }
    
    int currentIndex = 0;
    for (auto& node : list.forwardNodes()) {
        if (currentIndex == 0 || currentIndex == nodeCount - 1) {
            auto nodePtr = &node;
            list.remove(nodePtr);
        }
        currentIndex++;
    }
    
    // Should have 2, 3, 4 remaining
    int expected[] = {2, 3, 4};
    int i = 0;
    for (auto& val : list.forward()) {
        ASSERT_EQ(val, expected[i++]);
    }
    ASSERT_EQ(i, 3);
}