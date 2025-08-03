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

    ASSERT_NO_ALLOCS();
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

    ASSERT_NO_ALLOCS();
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

    ASSERT_NO_ALLOCS();
}

TEST(intrusiveBSTEraseStructuralIntegrity) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    // Create a more complex tree structure to stress swapNodes
    //       50
    //     /    \
    //   30      70
    //  /  \    /  \
    // 20  40  60  80
    //    /      \   \
    //   35      65  90
    //              /
    //             85
    
    IntrusiveTestNode n50(50), n30(30), n70(70);
    IntrusiveTestNode n20(20), n40(40), n60(60), n80(80);
    IntrusiveTestNode n35(35), n65(65), n90(90), n85(85);
    
    bst.insert(&n50);
    bst.insert(&n30); bst.insert(&n70);
    bst.insert(&n20); bst.insert(&n40); bst.insert(&n60); bst.insert(&n80);
    bst.insert(&n35); bst.insert(&n65); bst.insert(&n90);
    bst.insert(&n85);
    
    // Helper lambda to verify BST property via in-order traversal
    auto verifyBSTProperty = [&bst]() {
        Vector<int> values;
        bst.visitDepthFirstInOrder([&values](const IntrusiveTestNode& node) {
            values.push(node.value);
        });
        
        // Check that values are in sorted order
        for (size_t i = 1; i < values.getSize(); i++) {
            if (values[i-1] >= values[i]) {
                return false;
            }
        }
        return true;
    };
    
    // Test 1: Erase leaf node
    ASSERT_EQ(bst.erase(85), &n85);
    ASSERT_TRUE(verifyBSTProperty());
    ASSERT_EQ(bst.find(85), nullptr);
    ASSERT_EQ(bst.find(90), &n90); // Parent should still be there
    
    // Test 2: Erase node with only left child
    ASSERT_EQ(bst.erase(40), &n40);
    ASSERT_TRUE(verifyBSTProperty());
    ASSERT_EQ(bst.find(40), nullptr);
    ASSERT_EQ(bst.find(35), &n35); // Child should still be there
    ASSERT_EQ(bst.find(30), &n30); // Parent should still be there
    
    // Test 3: Erase node with only right child  
    ASSERT_EQ(bst.erase(60), &n60);
    ASSERT_TRUE(verifyBSTProperty());
    ASSERT_EQ(bst.find(60), nullptr);
    ASSERT_EQ(bst.find(65), &n65); // Child should still be there
    ASSERT_EQ(bst.find(70), &n70); // Parent should still be there
    
    // Test 4: Erase node with both children (this should stress swapNodes)
    ASSERT_EQ(bst.erase(30), &n30);
    ASSERT_TRUE(verifyBSTProperty());
    ASSERT_EQ(bst.find(30), nullptr);
    ASSERT_EQ(bst.find(20), &n20); // Left child should still be there
    ASSERT_EQ(bst.find(35), &n35); // Right subtree should still be there
    
    // Test 5: Erase root node (complex swapNodes case)
    ASSERT_EQ(bst.erase(50), &n50);
    ASSERT_TRUE(verifyBSTProperty());
    ASSERT_EQ(bst.find(50), nullptr);
    // All other nodes should still be findable
    ASSERT_EQ(bst.find(20), &n20);
    ASSERT_EQ(bst.find(35), &n35);
    ASSERT_EQ(bst.find(65), &n65);
    ASSERT_EQ(bst.find(70), &n70);
    ASSERT_EQ(bst.find(80), &n80);
    ASSERT_EQ(bst.find(90), &n90);
}

TEST(intrusiveBSTEraseSuccessorCases) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    // Test case where successor is immediate right child
    //   10
    //  /  \
    // 5   15
    //      \
    //      20
    
    IntrusiveTestNode n10(10), n5(5), n15(15), n20(20);
    bst.insert(&n10);
    bst.insert(&n5);
    bst.insert(&n15);
    bst.insert(&n20);
    
    // Erase 10 - successor (15) is immediate right child
    ASSERT_EQ(bst.erase(10), &n10);
    
    // Verify structure is maintained
    Vector<int> values;
    bst.visitDepthFirstInOrder([&values](const IntrusiveTestNode& node) {
        values.push(node.value);
    });
    
    ASSERT_EQ(values.getSize(), 3);
    ASSERT_EQ(values[0], 5);
    ASSERT_EQ(values[1], 15);
    ASSERT_EQ(values[2], 20);
    
    // Test case where successor is deep in right subtree
    IntrusiveTestNode n25(25), n12(12), n18(18), n14(14), n16(16);
    bst.insert(&n25);
    bst.insert(&n12);
    bst.insert(&n18);
    bst.insert(&n14);
    bst.insert(&n16);
    
    // Current tree: 5, 12, 14, 15, 16, 18, 20, 25
    // Erase 12 - successor (14) is deeper in right subtree
    ASSERT_EQ(bst.erase(12), &n12);
    
    values.~Vector();
    new(&values) Vector<int>();
    bst.visitDepthFirstInOrder([&values](const IntrusiveTestNode& node) {
        values.push(node.value);
    });
    
    // Should be: 5, 14, 15, 16, 18, 20, 25
    ASSERT_EQ(values.getSize(), 7);
    for (size_t i = 1; i < values.getSize(); i++) {
        ASSERT_LT(values[i-1], values[i]); // Verify still sorted
    }
}

TEST(intrusiveBSTEraseAllNodes) {
    IntrusiveBinarySearchTree<IntrusiveTestNode, IntrusiveTestExtractor> bst;
    
    // Create nodes
    Vector<IntrusiveTestNode*> nodes;
    for (int i = 1; i <= 15; i++) {
        nodes.push(new IntrusiveTestNode(i));
        bst.insert(nodes[i-1]);
    }
    
    // Erase all nodes in random order to stress swapNodes thoroughly
    Vector<int> eraseOrder = {8, 3, 12, 1, 15, 6, 10, 4, 13, 7, 2, 11, 9, 5, 14};
    
    for (size_t i = 0; i < eraseOrder.getSize(); i++) {
        int valueToErase = eraseOrder[i];
        IntrusiveTestNode* erased = bst.erase(valueToErase);
        ASSERT_NE(erased, nullptr);
        ASSERT_EQ(erased->value, valueToErase);
        
        // Verify BST property is maintained after each erase
        Vector<int> remaining;
        bst.visitDepthFirstInOrder([&remaining](const IntrusiveTestNode& node) {
            remaining.push(node.value);
        });
        
        // Check that remaining values are still in sorted order
        for (size_t j = 1; j < remaining.getSize(); j++) {
            ASSERT_LT(remaining[j-1], remaining[j]);
        }
        
        // Verify the erased value is no longer findable
        ASSERT_EQ(bst.find(valueToErase), nullptr);
    }
    
    // Clean up
    for (auto* node : nodes) {
        delete node;
    }
    
    // Tree should be empty now
    Vector<int> final;
    bst.visitDepthFirstInOrder([&final](const IntrusiveTestNode& node) {
        final.push(node.value);
    });
    ASSERT_EQ(final.getSize(), 0);
}

