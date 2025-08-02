//
// Created by Spencer Martin on 8/1/25.
//

#include "../test.h"
#include "../harness/TestHarness.h"

#include <core/ds/Trees.h>
#include <core/ds/Vector.h>

using namespace CroCOSTest;

TEST(binaryTreeConstruction) {
    // Test empty tree
    BinaryTree<int> emptyTree;
    ASSERT_TRUE(emptyTree.empty());
    ASSERT_EQ(emptyTree.getRoot(), nullptr);
    
    // Test tree with root value
    BinaryTree<int> tree(42);
    ASSERT_FALSE(tree.empty());
    ASSERT_NE(tree.getRoot(), nullptr);
    ASSERT_EQ(tree.getRoot()->data, 42);
    ASSERT_EQ(tree.getRoot()->left, nullptr);
    ASSERT_EQ(tree.getRoot()->right, nullptr);
}

TEST(binaryTreeManualConstruction) {
    BinaryTree<int> tree;
    tree.setRoot(10);
    
    auto* root = tree.getRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->data, 10);
    
    // Add children
    tree.setLeftChild(root, 5);
    tree.setRightChild(root, 15);
    
    ASSERT_NE(root->left, nullptr);
    ASSERT_NE(root->right, nullptr);
    ASSERT_EQ(root->left->data, 5);
    ASSERT_EQ(root->right->data, 15);
}

TEST(binaryTreeTraversal) {
    BinaryTree<int> tree(10);
    auto* root = tree.getRoot();
    tree.setLeftChild(root, 5);
    tree.setRightChild(root, 15);
    tree.setLeftChild(root->left, 3);
    tree.setRightChild(root->left, 7);
    
    // In-order traversal: 3, 5, 7, 10, 15
    Vector<int> inOrder;
    tree.visitDepthFirstInOrder([&inOrder](const TreeNode<int>& node) {
        inOrder.push(node.data);
    });
    
    ASSERT_EQ(inOrder.getSize(), 5);
    ASSERT_EQ(inOrder[0], 3);
    ASSERT_EQ(inOrder[1], 5);
    ASSERT_EQ(inOrder[2], 7);
    ASSERT_EQ(inOrder[3], 10);
    ASSERT_EQ(inOrder[4], 15);
    
    // Post-order traversal: 3, 7, 5, 15, 10
    Vector<int> postOrder;
    tree.visitDepthFirstPostOrder([&postOrder](const TreeNode<int>& node) {
        postOrder.push(node.data);
    });
    
    ASSERT_EQ(postOrder.getSize(), 5);
    ASSERT_EQ(postOrder[0], 3);
    ASSERT_EQ(postOrder[1], 7);
    ASSERT_EQ(postOrder[2], 5);
    ASSERT_EQ(postOrder[3], 15);
    ASSERT_EQ(postOrder[4], 10);
}

TEST(binarySearchTreeBasicOperations) {
    BinarySearchTree<int> bst;
    
    // Test empty tree
    ASSERT_TRUE(bst.empty());
    ASSERT_FALSE(bst.contains(5));
    ASSERT_FALSE(bst.erase(5));
    
    // Test insertion
    bst.insert(5);
    bst.insert(3);
    bst.insert(7);
    bst.insert(1);
    bst.insert(9);
    
    ASSERT_FALSE(bst.empty());
    
    // Test find
    ASSERT_TRUE(bst.contains(5));
    ASSERT_TRUE(bst.contains(3));
    ASSERT_TRUE(bst.contains(7));
    ASSERT_TRUE(bst.contains(1));
    ASSERT_TRUE(bst.contains(9));
    ASSERT_FALSE(bst.contains(2));
    ASSERT_FALSE(bst.contains(8));
    
    // Test erase
    ASSERT_TRUE(bst.erase(3));
    ASSERT_FALSE(bst.contains(3));
    ASSERT_FALSE(bst.erase(3)); // Second erase should fail
    
    // Remaining elements should still be there
    ASSERT_TRUE(bst.contains(5));
    ASSERT_TRUE(bst.contains(7));
    ASSERT_TRUE(bst.contains(1));
    ASSERT_TRUE(bst.contains(9));
}

TEST(binarySearchTreeInOrderTraversal) {
    BinarySearchTree<int> bst;
    
    // Insert values out of order
    bst.insert(5);
    bst.insert(3);
    bst.insert(7);
    bst.insert(1);
    bst.insert(9);
    bst.insert(4);
    bst.insert(6);
    
    // In-order traversal should give sorted sequence
    Vector<int> result;
    bst.visitDepthFirstInOrder([&result](const TreeNode<int>& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.getSize(), 7);
    ASSERT_EQ(result[0], 1);
    ASSERT_EQ(result[1], 3);
    ASSERT_EQ(result[2], 4);
    ASSERT_EQ(result[3], 5);
    ASSERT_EQ(result[4], 6);
    ASSERT_EQ(result[5], 7);
    ASSERT_EQ(result[6], 9);
}

TEST(binarySearchTreeFloorCeil) {
    BinarySearchTree<int> bst;
    
    bst.insert(2);
    bst.insert(4);
    bst.insert(6);
    bst.insert(8);
    
    int result;
    
    // Test floor (largest element <= value)
    ASSERT_FALSE(bst.floor(1, result));        // No element <= 1
    ASSERT_TRUE(bst.floor(2, result));         // Exact match
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(bst.floor(3, result));         // Largest <= 3 is 2
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(bst.floor(5, result));         // Largest <= 5 is 4
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(bst.floor(8, result));         // Exact match
    ASSERT_EQ(result, 8);
    ASSERT_TRUE(bst.floor(10, result));        // Largest <= 10 is 8
    ASSERT_EQ(result, 8);
    
    // Test ceil (smallest element >= value)
    ASSERT_TRUE(bst.ceil(1, result));          // Smallest >= 1 is 2
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(bst.ceil(2, result));          // Exact match
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(bst.ceil(3, result));          // Smallest >= 3 is 4
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(bst.ceil(5, result));          // Smallest >= 5 is 6
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(bst.ceil(8, result));          // Exact match
    ASSERT_EQ(result, 8);
    ASSERT_FALSE(bst.ceil(9, result));         // No element >= 9
}

TEST(binarySearchTreeSuccessorPredecessor) {
    BinarySearchTree<int> bst;
    
    bst.insert(5);
    bst.insert(3);
    bst.insert(7);
    bst.insert(1);
    bst.insert(9);
    bst.insert(4);
    bst.insert(6);
    
    int result;
    
    // Test successor
    ASSERT_TRUE(bst.successor(1, result));
    ASSERT_EQ(result, 3);
    ASSERT_TRUE(bst.successor(3, result));
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(bst.successor(4, result));
    ASSERT_EQ(result, 5);
    ASSERT_TRUE(bst.successor(5, result));
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(bst.successor(6, result));
    ASSERT_EQ(result, 7);
    ASSERT_TRUE(bst.successor(7, result));
    ASSERT_EQ(result, 9);
    ASSERT_FALSE(bst.successor(9, result));    // Largest element
    
    // Test predecessor
    ASSERT_FALSE(bst.predecessor(1, result));  // Smallest element
    ASSERT_TRUE(bst.predecessor(3, result));
    ASSERT_EQ(result, 1);
    ASSERT_TRUE(bst.predecessor(4, result));
    ASSERT_EQ(result, 3);
    ASSERT_TRUE(bst.predecessor(5, result));
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(bst.predecessor(6, result));
    ASSERT_EQ(result, 5);
    ASSERT_TRUE(bst.predecessor(7, result));
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(bst.predecessor(9, result));
    ASSERT_EQ(result, 7);
}

TEST(binarySearchTreeMoveSemantics) {
    BinarySearchTree<int> bst1;
    bst1.insert(5);
    bst1.insert(3);
    bst1.insert(7);
    
    // Move constructor
    BinarySearchTree<int> bst2 = move(bst1);
    ASSERT_TRUE(bst1.empty());
    ASSERT_FALSE(bst2.empty());
    ASSERT_TRUE(bst2.contains(5));
    ASSERT_TRUE(bst2.contains(3));
    ASSERT_TRUE(bst2.contains(7));
    
    // Move assignment
    BinarySearchTree<int> bst3;
    bst3.insert(10);
    bst3 = move(bst2);
    ASSERT_TRUE(bst2.empty());
    ASSERT_FALSE(bst3.empty());
    ASSERT_TRUE(bst3.contains(5));
    ASSERT_FALSE(bst3.contains(10)); // Old content should be destroyed
}

// Simple intrusive node for testing
struct IntrusiveTestNode {
    int value;
    IntrusiveTestNode* left;
    IntrusiveTestNode* right;
    
    IntrusiveTestNode(int val) : value(val), left(nullptr), right(nullptr) {}
};

struct IntrusiveTestExtractor {
    static IntrusiveTestNode*& left(IntrusiveTestNode& node) { return node.left; }
    static IntrusiveTestNode*& right(IntrusiveTestNode& node) { return node.right; }
    static IntrusiveTestNode* const& left(const IntrusiveTestNode& node) { return node.left; }
    static IntrusiveTestNode* const& right(const IntrusiveTestNode& node) { return node.right; }
    static int& data(IntrusiveTestNode& node) { return node.value; }
    static const int& data(const IntrusiveTestNode& node) { return node.value; }
};

TEST(intrusiveBinarySearchTreeBasics) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    // Create nodes
    IntrusiveTestNode n5(5);
    IntrusiveTestNode n3(3);
    IntrusiveTestNode n7(7);
    IntrusiveTestNode n1(1);
    IntrusiveTestNode n9(9);
    
    // Insert nodes
    bst.insert(&n5);
    bst.insert(&n3);
    bst.insert(&n7);
    bst.insert(&n1);
    bst.insert(&n9);
    
    // Test find
    ASSERT_EQ(bst.find(5), &n5);
    ASSERT_EQ(bst.find(3), &n3);
    ASSERT_EQ(bst.find(7), &n7);
    ASSERT_EQ(bst.find(1), &n1);
    ASSERT_EQ(bst.find(9), &n9);
    ASSERT_EQ(bst.find(2), nullptr);
    
    // Test erase
    IntrusiveTestNode* erased = bst.erase(3);
    ASSERT_EQ(erased, &n3);
    ASSERT_EQ(bst.find(3), nullptr);
    
    // Other nodes should still be findable
    ASSERT_EQ(bst.find(5), &n5);
    ASSERT_EQ(bst.find(7), &n7);
    ASSERT_EQ(bst.find(1), &n1);
    ASSERT_EQ(bst.find(9), &n9);
}

TEST(intrusiveBSTSuccessorPredecessor) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    IntrusiveTestNode n5(5);
    IntrusiveTestNode n3(3);
    IntrusiveTestNode n7(7);
    IntrusiveTestNode n1(1);
    IntrusiveTestNode n9(9);
    IntrusiveTestNode n4(4);
    IntrusiveTestNode n6(6);
    
    bst.insert(&n5);
    bst.insert(&n3);
    bst.insert(&n7);
    bst.insert(&n1);
    bst.insert(&n9);
    bst.insert(&n4);
    bst.insert(&n6);
    
    // Test successor
    ASSERT_EQ(bst.successor(&n1), &n3);
    ASSERT_EQ(bst.successor(&n3), &n4);
    ASSERT_EQ(bst.successor(&n4), &n5);
    ASSERT_EQ(bst.successor(&n5), &n6);
    ASSERT_EQ(bst.successor(&n6), &n7);
    ASSERT_EQ(bst.successor(&n7), &n9);
    ASSERT_EQ(bst.successor(&n9), nullptr); // Largest element
    
    // Test predecessor
    ASSERT_EQ(bst.predecessor(&n9), &n7);
    ASSERT_EQ(bst.predecessor(&n7), &n6);
    ASSERT_EQ(bst.predecessor(&n6), &n5);
    ASSERT_EQ(bst.predecessor(&n5), &n4);
    ASSERT_EQ(bst.predecessor(&n4), &n3);
    ASSERT_EQ(bst.predecessor(&n3), &n1);
    ASSERT_EQ(bst.predecessor(&n1), nullptr); // Smallest element
}

TEST(intrusiveBSTFloorCeil) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    IntrusiveTestNode n2(2);
    IntrusiveTestNode n4(4);
    IntrusiveTestNode n6(6);
    IntrusiveTestNode n8(8);
    
    bst.insert(&n2);
    bst.insert(&n4);
    bst.insert(&n6);
    bst.insert(&n8);
    
    // Test floor (largest element <= value)
    ASSERT_EQ(bst.floor(1), nullptr);     // No element <= 1
    ASSERT_EQ(bst.floor(2), &n2);         // Exact match
    ASSERT_EQ(bst.floor(3), &n2);         // Largest <= 3 is 2
    ASSERT_EQ(bst.floor(5), &n4);         // Largest <= 5 is 4
    ASSERT_EQ(bst.floor(8), &n8);         // Exact match
    ASSERT_EQ(bst.floor(10), &n8);        // Largest <= 10 is 8
    
    // Test ceil (smallest element >= value)
    ASSERT_EQ(bst.ceil(1), &n2);          // Smallest >= 1 is 2
    ASSERT_EQ(bst.ceil(2), &n2);          // Exact match
    ASSERT_EQ(bst.ceil(3), &n4);          // Smallest >= 3 is 4
    ASSERT_EQ(bst.ceil(5), &n6);          // Smallest >= 5 is 6
    ASSERT_EQ(bst.ceil(8), &n8);          // Exact match
    ASSERT_EQ(bst.ceil(9), nullptr);      // No element >= 9
}

