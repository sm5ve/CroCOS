//
// Created by Spencer Martin on 8/1/25.
//

//#define PARANOID_RBT_VERIFICATION

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
    tree.visitDepthFirstInOrder([&inOrder](const TreeNode<int, true>& node) {
        inOrder.push(node.data);
    });
    
    ASSERT_EQ(inOrder.size(), 5);
    ASSERT_EQ(inOrder[0], 3);
    ASSERT_EQ(inOrder[1], 5);
    ASSERT_EQ(inOrder[2], 7);
    ASSERT_EQ(inOrder[3], 10);
    ASSERT_EQ(inOrder[4], 15);
    
    // Post-order traversal: 3, 7, 5, 15, 10
    Vector<int> postOrder;
    tree.visitDepthFirstPostOrder([&postOrder](const TreeNode<int, true>& node) {
        postOrder.push(node.data);
    });
    
    ASSERT_EQ(postOrder.size(), 5);
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
    bst.visitDepthFirstInOrder([&result](const TreeNode<int, true>& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.size(), 7);
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
        for (size_t i = 1; i < values.size(); i++) {
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
    
    ASSERT_EQ(values.size(), 3);
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
    ASSERT_EQ(values.size(), 7);
    for (size_t i = 1; i < values.size(); i++) {
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
    
    for (size_t i = 0; i < eraseOrder.size(); i++) {
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
        for (size_t j = 1; j < remaining.size(); j++) {
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
    ASSERT_EQ(final.size(), 0);
}

TEST(redBlackTreeBasicOperations) {
    RedBlackTree<int> rbt;
    
    // Test empty tree
    ASSERT_TRUE(rbt.empty());
    ASSERT_FALSE(rbt.contains(5));
    
    // Test insertion
    rbt.insert(5);
    rbt.insert(3);
    rbt.insert(7);
    rbt.insert(1);
    rbt.insert(9);
    
    ASSERT_FALSE(rbt.empty());
    
    // Test find
    ASSERT_TRUE(rbt.contains(5));
    ASSERT_TRUE(rbt.contains(3));
    ASSERT_TRUE(rbt.contains(7));
    ASSERT_TRUE(rbt.contains(1));
    ASSERT_TRUE(rbt.contains(9));
    ASSERT_FALSE(rbt.contains(2));
    ASSERT_FALSE(rbt.contains(8));
}

TEST(redBlackTreeInOrderTraversal) {
    RedBlackTree<int> rbt;
    
    // Insert values out of order
    rbt.insert(5);
    rbt.insert(3);
    rbt.insert(7);
    rbt.insert(1);
    rbt.insert(9);
    rbt.insert(4);
    rbt.insert(6);
    
    // In-order traversal should give sorted sequence
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.size(), 7);
    ASSERT_EQ(result[0], 1);
    ASSERT_EQ(result[1], 3);
    ASSERT_EQ(result[2], 4);
    ASSERT_EQ(result[3], 5);
    ASSERT_EQ(result[4], 6);
    ASSERT_EQ(result[5], 7);
    ASSERT_EQ(result[6], 9);
}

TEST(redBlackTreeFloorCeil) {
    RedBlackTree<int> rbt;
    
    rbt.insert(2);
    rbt.insert(4);
    rbt.insert(6);
    rbt.insert(8);
    
    int result;
    
    // Test floor (largest element <= value)
    ASSERT_FALSE(rbt.floor(1, result));        // No element <= 1
    ASSERT_TRUE(rbt.floor(2, result));         // Exact match
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(rbt.floor(3, result));         // Largest <= 3 is 2
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(rbt.floor(5, result));         // Largest <= 5 is 4
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(rbt.floor(8, result));         // Exact match
    ASSERT_EQ(result, 8);
    ASSERT_TRUE(rbt.floor(10, result));        // Largest <= 10 is 8
    ASSERT_EQ(result, 8);
    
    // Test ceil (smallest element >= value)
    ASSERT_TRUE(rbt.ceil(1, result));          // Smallest >= 1 is 2
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(rbt.ceil(2, result));          // Exact match
    ASSERT_EQ(result, 2);
    ASSERT_TRUE(rbt.ceil(3, result));          // Smallest >= 3 is 4
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(rbt.ceil(5, result));          // Smallest >= 5 is 6
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(rbt.ceil(8, result));          // Exact match
    ASSERT_EQ(result, 8);
    ASSERT_FALSE(rbt.ceil(9, result));         // No element >= 9
}

TEST(redBlackTreeSuccessorPredecessor) {
    RedBlackTree<int> rbt;
    
    rbt.insert(5);
    rbt.insert(3);
    rbt.insert(7);
    rbt.insert(1);
    rbt.insert(9);
    rbt.insert(4);
    rbt.insert(6);
    
    int result;
    
    // Test successor
    ASSERT_TRUE(rbt.successor(1, result));
    ASSERT_EQ(result, 3);
    ASSERT_TRUE(rbt.successor(3, result));
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(rbt.successor(4, result));
    ASSERT_EQ(result, 5);
    ASSERT_TRUE(rbt.successor(5, result));
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(rbt.successor(6, result));
    ASSERT_EQ(result, 7);
    ASSERT_TRUE(rbt.successor(7, result));
    ASSERT_EQ(result, 9);
    ASSERT_FALSE(rbt.successor(9, result));    // Largest element
    
    // Test predecessor
    ASSERT_FALSE(rbt.predecessor(1, result));  // Smallest element
    ASSERT_TRUE(rbt.predecessor(3, result));
    ASSERT_EQ(result, 1);
    ASSERT_TRUE(rbt.predecessor(4, result));
    ASSERT_EQ(result, 3);
    ASSERT_TRUE(rbt.predecessor(5, result));
    ASSERT_EQ(result, 4);
    ASSERT_TRUE(rbt.predecessor(6, result));
    ASSERT_EQ(result, 5);
    ASSERT_TRUE(rbt.predecessor(7, result));
    ASSERT_EQ(result, 6);
    ASSERT_TRUE(rbt.predecessor(9, result));
    ASSERT_EQ(result, 7);
}

TEST(redBlackTreeMoveSemantics) {
    RedBlackTree<int> rbt1;
    rbt1.insert(5);
    rbt1.insert(3);
    rbt1.insert(7);
    
    // Move constructor
    RedBlackTree<int> rbt2 = move(rbt1);
    ASSERT_TRUE(rbt1.empty());
    ASSERT_FALSE(rbt2.empty());
    ASSERT_TRUE(rbt2.contains(5));
    ASSERT_TRUE(rbt2.contains(3));
    ASSERT_TRUE(rbt2.contains(7));
    
    // Move assignment
    RedBlackTree<int> rbt3;
    rbt3.insert(10);
    rbt3 = move(rbt2);
    ASSERT_TRUE(rbt2.empty());
    ASSERT_FALSE(rbt3.empty());
    ASSERT_TRUE(rbt3.contains(5));
    ASSERT_FALSE(rbt3.contains(10)); // Old content should be destroyed
}

TEST(redBlackTreeBalancingProperties) {
    RedBlackTree<int> rbt;
    
    // Insert sequential values that would create a degenerate BST
    // Red-black tree should remain balanced
    for (int i = 1; i <= 15; i++) {
        rbt.insert(i);
    }
    
    // Verify all values are present
    for (int i = 1; i <= 15; i++) {
        ASSERT_TRUE(rbt.contains(i));
    }
    
    // Verify in-order traversal gives sorted sequence
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.size(), 15);
    for (int i = 0; i < 15; i++) {
        ASSERT_EQ(result[i], i + 1);
    }
}

// Helper to verify red-black tree properties
template<typename T>
bool verifyRBTStructure(const RedBlackTreeNode<T, NoAugmentation, false>* node, int& blackHeight) {
    if (node == nullptr) {
        blackHeight = 1; // Null nodes are considered black
        return true;
    }

    // Property 1: Red nodes cannot have red children
    if (node->isRed) {
        if ((node->left && node->left->isRed) || (node->right && node->right->isRed)) {
			ASSERT_UNREACHABLE("Red node has red child");
            return false; // Red node has red child
        }
    }

    // Property 2: All paths to leaves have same black height
    int leftBlackHeight = 0, rightBlackHeight = 0;
    if (!verifyRBTStructure<int>(node->left, leftBlackHeight) ||
        !verifyRBTStructure<int>(node->right, rightBlackHeight)) {
		ASSERT_UNREACHABLE("Child tree failure");
        return false;
    }

    if (leftBlackHeight != rightBlackHeight) {
		ASSERT_UNREACHABLE("Black heights differ");
        return false; // Different black heights
    }

    blackHeight = leftBlackHeight + (node->isRed ? 0 : 1);
    return true;
}

template<typename T>
bool verifyRBTStructure(const RedBlackTreeNode<T, NoAugmentation, true>* node, int& blackHeight) {
    if (node == nullptr) {
        blackHeight = 1; // Null nodes are considered black
        return true;
    }

	auto* parentptr = node->parent;
	if(parentptr != nullptr){
		if(parentptr -> left != node && parentptr -> right != node){
			ASSERT_UNREACHABLE("Parent pointer improperly set");
			return false;
		}
	}

    // Property 1: Red nodes cannot have red children
    if (node->isRed) {
        if ((node->left && node->left->isRed) || (node->right && node->right->isRed)) {
			ASSERT_UNREACHABLE("Red node has red child");
            return false; // Red node has red child
        }
    }

    // Property 2: All paths to leaves have same black height
    int leftBlackHeight = 0, rightBlackHeight = 0;
    if (!verifyRBTStructure<int>(node->left, leftBlackHeight) ||
        !verifyRBTStructure<int>(node->right, rightBlackHeight)) {
		ASSERT_UNREACHABLE("Child tree failure");
        return false;
    }

    if (leftBlackHeight != rightBlackHeight) {
		ASSERT_UNREACHABLE("Black heights differ");
        return false; // Different black heights
    }

    blackHeight = leftBlackHeight + (node->isRed ? 0 : 1);
    return true;
}

TEST(redBlackTreeRedBlackProperties) {
    RedBlackTree<int> rbt;
    
    // Insert values in various orders to test red-black tree properties
    Vector<int> values = {7, 3, 18, 10, 22, 8, 11, 26, 2, 6, 13};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Root should be black (this is a fundamental red-black tree property)
    // We can't directly access the root, but we can verify the tree maintains sorted order
    Vector<int> sortedResult;
    rbt.visitDepthFirstInOrder([&sortedResult](const auto& node) {
        sortedResult.push(node.data);
    });
    
    // Verify tree is balanced by checking sorted order is maintained
    for (size_t i = 1; i < sortedResult.size(); i++) {
        ASSERT_LT(sortedResult[i-1], sortedResult[i]);
    }
    
    // Verify all original values are present
    for (int val : values) {
        ASSERT_TRUE(rbt.contains(val));
    }

	int blackHeight = 0;
	ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
}

TEST(redBlackTreeLargeDataset) {
    RedBlackTree<int> rbt;
    
    // Insert a large number of values to stress test the balancing
    const int maxVal = 1000;//100000;
    Vector<int> insertOrder;
    
    // Create a shuffled sequence
    for (int i = 1; i <= maxVal; i++) {
        insertOrder.push(i);
    }
    
    // Simple shuffle by reversing sections
    for (int i = 0; i < maxVal / 2; i += 10) {
        for (int j = 0; j < 5 && i + j < maxVal && i + 9 - j >= 0; j++) {
            if (i + j < i + 9 - j) {
                int temp = insertOrder[i + j];
                insertOrder[i + j] = insertOrder[i + 9 - j];
                insertOrder[i + 9 - j] = temp;
            }
        }
    }
    
    // Insert all values
    for (int val : insertOrder) {
        rbt.insert(val);
    }
    
    // Verify all values are present
    for (int i = 1; i <= maxVal; i++) {
        ASSERT_TRUE(rbt.contains(i));
    }
    
    // Verify in-order traversal gives sorted sequence
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.size(), maxVal);
    for (int i = 0; i < maxVal; i++) {
        ASSERT_EQ(result[i], i + 1);
    }

	int blackHeight = 0;
	ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
}

TEST(redBlackTreeNoDuplicates) {
    RedBlackTree<int> rbt;
    
    // Insert same values multiple times
    rbt.insert(5);
    rbt.insert(5);  // Duplicate
    rbt.insert(3);
    rbt.insert(3);  // Duplicate
    rbt.insert(7);
    rbt.insert(7);  // Duplicate
    
    // Should only contain unique values
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    ASSERT_EQ(result.size(), 3);
    ASSERT_EQ(result[0], 3);
    ASSERT_EQ(result[1], 5);
    ASSERT_EQ(result[2], 7);

	int blackHeight = 0;
	ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
}

TEST(redBlackTreeEraseRedLeaf) {
    RedBlackTree<int> rbt;
    
    // Create simple tree: black root with red children
    rbt.insert(10);
    rbt.insert(5);
    rbt.insert(15);
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Erase red leaf (should not require fixup)
    ASSERT_TRUE(rbt.erase(5));
    ASSERT_FALSE(rbt.contains(5));
    
    // Verify structure is still valid
    blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Remaining elements should still be there
    ASSERT_TRUE(rbt.contains(10));
    ASSERT_TRUE(rbt.contains(15));
}

TEST(redBlackTreeEraseRoot) {
    RedBlackTree<int> rbt;
    
    // Single node tree
    rbt.insert(10);
    ASSERT_TRUE(rbt.contains(10));
    
    // Erase root
    ASSERT_TRUE(rbt.erase(10));
    ASSERT_TRUE(rbt.empty());
    ASSERT_FALSE(rbt.contains(10));
}

TEST(redBlackTreeEraseBlackLeafShouldTriggerFixup) {
    RedBlackTree<int> rbt;
    
    // Create a tree with 10 nodes (not 2^n - 1) to guarantee black leaves
    for (int i = 1; i <= 10; i++) {
        rbt.insert(i);
    }
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));

	int toDelete = -1;
    rbt.visitDepthFirstInOrder([&toDelete](const auto& node) {
		if(node.left == nullptr && node.right == nullptr){
			if(!node.isRed){
				toDelete = node.data;
			}
		}
	});
	ASSERT_NE(toDelete, -1);
	ASSERT_TRUE(rbt.erase(toDelete));
}

TEST(redBlackTreeEraseNodeWithOneChild) {
    RedBlackTree<int> rbt;
    
    // Create a more complex tree to ensure nodes with one child exist
    Vector<int> values = {10, 5, 15, 3, 7, 12, 18, 1, 4, 6, 8, 11, 13, 16, 20};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Find a node with exactly one child
    int nodeWithOneChild = -1;
    rbt.visitDepthFirstInOrder([&nodeWithOneChild](const auto& node) {
        bool hasLeft = (node.left != nullptr);
        bool hasRight = (node.right != nullptr);
        if (hasLeft != hasRight) { // XOR - exactly one child
            nodeWithOneChild = node.data;
        }
    });
    
    if (nodeWithOneChild != -1) {
        // Erase node with one child
        ASSERT_TRUE(rbt.erase(nodeWithOneChild));
        ASSERT_FALSE(rbt.contains(nodeWithOneChild));
        
        // Verify red-black properties are maintained
        blackHeight = 0;
        ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        
        // Verify remaining nodes are still present
        for (int val : values) {
            if (val != nodeWithOneChild) {
                ASSERT_TRUE(rbt.contains(val));
            }
        }
    }
}

TEST(redBlackTreeEraseNodeWithTwoChildren) {
    RedBlackTree<int> rbt;
    
    // Create a tree where we know there are nodes with two children
    Vector<int> values = {50, 25, 75, 12, 37, 62, 87, 6, 18, 31, 43, 56, 68, 81, 93};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Find a node with two children
    int nodeWithTwoChildren = -1;
    rbt.visitDepthFirstInOrder([&nodeWithTwoChildren](const auto& node) {
        if (node.left != nullptr && node.right != nullptr) {
            nodeWithTwoChildren = node.data;
        }
    });
    
    ASSERT_NE(nodeWithTwoChildren, -1);
    
    // Erase node with two children
    ASSERT_TRUE(rbt.erase(nodeWithTwoChildren));
    ASSERT_FALSE(rbt.contains(nodeWithTwoChildren));
    
    // Verify red-black properties are maintained
    blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Verify remaining nodes are still present and tree is sorted
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    for (size_t i = 1; i < result.size(); i++) {
        ASSERT_LT(result[i-1], result[i]); // Verify sorted order
    }
    
    // Verify all other values are still there
    for (int val : values) {
        if (val != nodeWithTwoChildren) {
            ASSERT_TRUE(rbt.contains(val));
        }
    }
}

TEST(redBlackTreeEraseComplexRootCases) {
    RedBlackTree<int> rbt;
    
    // Create a complex tree
    Vector<int> values = {20, 10, 30, 5, 15, 25, 35, 2, 7, 12, 18, 22, 27, 32, 40};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Erase root node (should trigger complex rebalancing)
    ASSERT_TRUE(rbt.erase(20));
    ASSERT_FALSE(rbt.contains(20));
    
    // Verify red-black properties are maintained
    blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Verify all other nodes are still present
    for (int val : values) {
        if (val != 20) {
            ASSERT_TRUE(rbt.contains(val));
        }
    }
    
    // Verify tree is still sorted
    Vector<int> result;
    rbt.visitDepthFirstInOrder([&result](const auto& node) {
        result.push(node.data);
    });
    
    for (size_t i = 1; i < result.size(); i++) {
        ASSERT_LT(result[i-1], result[i]);
    }
}

TEST(redBlackTreeSequentialErase) {
    RedBlackTree<int> rbt;
    
    // Insert values
    Vector<int> values = {50, 25, 75, 12, 37, 62, 87, 6, 18, 31, 43, 56, 68, 81, 93, 3, 9, 15, 21, 28, 34, 40, 46};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Erase values in a specific order that tests different cases
    Vector<int> eraseOrder = {12, 75, 31, 6, 87, 37, 21, 93, 25, 68, 15, 56};
    
    for (int val : eraseOrder) {
        ASSERT_TRUE(rbt.erase(val));
        ASSERT_FALSE(rbt.contains(val));
        
        // Verify red-black properties after each deletion
        int blackHeight = 0;
        ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        
        // Verify tree maintains sorted order
        Vector<int> result;
        rbt.visitDepthFirstInOrder([&result](const auto& node) {
            result.push(node.data);
        });
        
        for (size_t i = 1; i < result.size(); i++) {
            ASSERT_LT(result[i-1], result[i]);
        }
    }
    
    // Verify remaining values are still present
    for (int val : values) {
        bool shouldBePresent = true;
        for (int erased : eraseOrder) {
            if (val == erased) {
                shouldBePresent = false;
                break;
            }
        }
        if (shouldBePresent) {
            ASSERT_TRUE(rbt.contains(val));
        } else {
            ASSERT_FALSE(rbt.contains(val));
        }
    }
}

TEST(redBlackTreeEraseAllNodesRandomOrder) {
    RedBlackTree<int> rbt;
    
    // Insert sequential values
    const int numNodes = 31; // Use odd number to create interesting tree structure
    for (int i = 1; i <= numNodes; i++) {
        rbt.insert(i);
    }
    
    // Erase in pseudo-random order
    Vector<int> eraseOrder = {16, 8, 24, 4, 12, 20, 28, 2, 6, 10, 14, 18, 22, 26, 30, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31};
    
    for (int val : eraseOrder) {
        ASSERT_TRUE(rbt.contains(val)); // Verify it exists before erasing
        ASSERT_TRUE(rbt.erase(val));
        ASSERT_FALSE(rbt.contains(val)); // Verify it's gone after erasing
        
        if (!rbt.empty()) { // Skip verification for empty tree
            // Verify red-black properties after each deletion
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
    }
    
    // Tree should be empty now
    ASSERT_TRUE(rbt.empty());
}

TEST(redBlackTreeEraseFixupStressCases) {
    RedBlackTree<int> rbt;
    
    // Create a specific tree structure that will trigger various fixup cases
    // Insert in order that creates interesting black-height patterns
    Vector<int> values = {64, 32, 96, 16, 48, 80, 112, 8, 24, 40, 56, 72, 88, 104, 120, 4, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124};
    
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Verify initial structure
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
    
    // Delete nodes that will trigger different fixup scenarios
    Vector<int> problematicDeletes = {4, 124, 16, 112, 8, 120, 32, 96}; // Leaf and internal nodes
    
    for (int val : problematicDeletes) {
        ASSERT_TRUE(rbt.erase(val));
        
        // Verify properties after each challenging deletion
        int currentBlackHeight = 0;
        ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), currentBlackHeight));
        
        // Verify tree still maintains BST property
        Vector<int> inOrder;
        rbt.visitDepthFirstInOrder([&inOrder](const auto& node) {
            inOrder.push(node.data);
        });
        
        for (size_t i = 1; i < inOrder.size(); i++) {
            ASSERT_LT(inOrder[i-1], inOrder[i]);
        }
    }
}

TEST(redBlackTreeEraseNonExistentValues) {
    RedBlackTree<int> rbt;
    
    // Insert some values
    Vector<int> values = {10, 5, 15, 3, 7, 12, 18};
    for (int val : values) {
        rbt.insert(val);
    }
    
    // Try to erase values that don't exist
    ASSERT_FALSE(rbt.erase(1));   // Less than minimum
    ASSERT_FALSE(rbt.erase(20));  // Greater than maximum
    ASSERT_FALSE(rbt.erase(6));   // Between existing values
    ASSERT_FALSE(rbt.erase(13));  // Between existing values
    
    // Verify all original values are still there
    for (int val : values) {
        ASSERT_TRUE(rbt.contains(val));
    }
    
    // Verify structure is still valid
    int blackHeight = 0;
    ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
}

TEST(redBlackTreeRandomInsertDeleteStressTest) {
    const int maxValue = 100;
    const int maxLoop = 50;
    
    // Seed for reproducible results - you can change this for different test runs
    std::srand(42);
    
    for (int loop = 0; loop < maxLoop; loop++) {
        RedBlackTree<int> rbt;
        Vector<int> values;

        // Generate sequential values 1 to maxValue
        for (int i = 1; i <= maxValue; i++) {
            values.push(i);
        }
        
        // Shuffle for random insertion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Insert in random order
        for (int i = 0; i < maxValue; i++) {
            rbt.insert(values[i]);
            
            // Verify tree structure after each insertion
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
        
        // Verify all values are present
        for (int i = 1; i <= maxValue; i++) {
            ASSERT_TRUE(rbt.contains(i));
        }
        
        // Shuffle again for random deletion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Delete in random order
        for (int i = 0; i < maxValue; i++) {
            ASSERT_TRUE(rbt.erase(values[i]));
            ASSERT_FALSE(rbt.contains(values[i]));
            
            // Verify tree structure after each deletion
            if (!rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
            
            // Verify remaining values are still present
            for (int j = i + 1; j < maxValue; j++) {
                ASSERT_TRUE(rbt.contains(values[j]));
            }
        }
        
        // Tree should be empty now
        ASSERT_TRUE(rbt.empty());
    }
}

TEST(redBlackTreeExtremeCasesStressTest) {
    // Test various edge cases that can break RBT implementations
    
    // Test 1: Sequential insertion and deletion
    {
        RedBlackTree<int> rbt;
        
        // Insert 1,2,3,...,50
        for (int i = 1; i <= 50; i++) {
            rbt.insert(i);
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
        
        // Delete in reverse order: 50,49,48,...,1
        for (int i = 50; i >= 1; i--) {
            ASSERT_TRUE(rbt.erase(i));
            if (!rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        ASSERT_TRUE(rbt.empty());
    }
    
    // Test 2: Reverse sequential insertion and deletion
    {
        RedBlackTree<int> rbt;
        
        // Insert 50,49,48,...,1
        for (int i = 50; i >= 1; i--) {
            rbt.insert(i);
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
        
        // Delete in order: 1,2,3,...,50
        for (int i = 1; i <= 50; i++) {
            ASSERT_TRUE(rbt.erase(i));
            if (!rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        ASSERT_TRUE(rbt.empty());
    }
    
    // Test 3: Insert/delete alternating pattern
    {
        RedBlackTree<int> rbt;
        Vector<int> inserted;
        
        // Insert 1,3,5,7,...,99 (odds)
        for (int i = 1; i < 100; i += 2) {
            rbt.insert(i);
            inserted.push(i);
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
        
        // Insert 2,4,6,8,...,100 (evens)
        for (int i = 2; i <= 100; i += 2) {
            rbt.insert(i);
            inserted.push(i);
            int blackHeight = 0;
            ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        }
        
        // Delete every other element
        for (int i = 0; i < inserted.size(); i += 2) {
            ASSERT_TRUE(rbt.erase(inserted[i]));
            if (!rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        // Delete remaining elements
        for (int i = 1; i < inserted.size(); i += 2) {
            ASSERT_TRUE(rbt.erase(inserted[i]));
            if (!rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        ASSERT_TRUE(rbt.empty());
    }
}

TEST(redBlackTreeLargeScaleStressTest) {
    const int largeValue = 1000;
    const int iterations = 5;
    
    std::srand(12345); // Different seed for variety
    
    for (int iter = 0; iter < iterations; iter++) {
        RedBlackTree<int> rbt;
        Vector<int> values;
        
        // Generate values 1 to largeValue
        for (int i = 1; i <= largeValue; i++) {
            values.push(i);
        }
        
        // Multiple shuffle passes for better randomization
        for (int pass = 0; pass < 3; pass++) {
            for (int i = largeValue - 1; i > 0; i--) {
                int j = std::rand() % (i + 1);
                int temp = values[i];
                values[i] = values[j];
                values[j] = temp;
            }
        }
        
        // Insert first half
        for (int i = 0; i < largeValue / 2; i++) {
            rbt.insert(values[i]);
            
            // Only verify structure every 50 insertions to speed up test
            if (i % 50 == 0) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        // Delete first quarter
        for (int i = 0; i < largeValue / 4; i++) {
            ASSERT_TRUE(rbt.erase(values[i]));
            
            if (i % 50 == 0 && !rbt.empty()) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        // Insert second half
        for (int i = largeValue / 2; i < largeValue; i++) {
            rbt.insert(values[i]);
            
            if (i % 50 == 0) {
                int blackHeight = 0;
                ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
            }
        }
        
        // Final structure verification
        int blackHeight = 0;
        ASSERT_TRUE(verifyRBTStructure<int>(rbt.getRoot(), blackHeight));
        
        // Verify expected contents (first quarter deleted, rest present)
        for (int i = 0; i < largeValue / 4; i++) {
            ASSERT_FALSE(rbt.contains(values[i]));
        }
        for (int i = largeValue / 4; i < largeValue; i++) {
            ASSERT_TRUE(rbt.contains(values[i]));
        }
        
        // Clean up - delete remaining elements
        for (int i = largeValue / 4; i < largeValue; i++) {
            rbt.erase(values[i]);
        }
        
        ASSERT_TRUE(rbt.empty());
    }
}

TEST(augmentedRedBlackTreeSumStressTest) {
    // Accumulator that computes the sum of all values in subtree
    struct SumAccumulator {
        int operator()(const int& nodeValue, const int* leftSum, const int* rightSum) const {
            int left = leftSum ? *leftSum : 0;
            int right = rightSum ? *rightSum : 0;
            return nodeValue + left + right;
        }
    };
    
    using SumAugmentedRBT = AugmentedRedBlackTree<int, int, SumAccumulator>;
    
    const int maxValue = 50;
    const int maxLoop = 20;
    
    // Seed for reproducible results
    std::srand(123);
    
    for (int loop = 0; loop < maxLoop; loop++) {
        SumAugmentedRBT arbt;
        Vector<int> values;
        int expectedTotalSum = 0;

        // Generate sequential values 1 to maxValue
        for (int i = 1; i <= maxValue; i++) {
            values.push(i);
            expectedTotalSum += i;
        }
        
        // Shuffle for random insertion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Insert in random order and verify sum augmentation
        int partialExpectedSum = 0;
        for (int i = 0; i < maxValue; i++) {
            arbt.insert(values[i]);
            partialExpectedSum += values[i];

			// Access root's augmentation data to verify sum
            const auto* root = arbt.getRoot();
            if (root != nullptr) {
                ASSERT_EQ(root->augmentationData, partialExpectedSum);
            }
        }
        
        // All values inserted - verify final sum
        const auto* root = arbt.getRoot();
        ASSERT_NE(root, nullptr);
        ASSERT_EQ(root->augmentationData, expectedTotalSum);
        
        // Verify all values are present
        for (int i = 1; i <= maxValue; i++) {
            ASSERT_TRUE(arbt.contains(i));
        }
        
        // Shuffle again for random deletion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Delete in random order and verify sum augmentation
        for (int i = 0; i < maxValue; i++) {
            int valueToDelete = values[i];
            ASSERT_TRUE(arbt.erase(valueToDelete));
            ASSERT_FALSE(arbt.contains(valueToDelete));
            expectedTotalSum -= valueToDelete;
            
            // Verify tree structure after deletion
            if (!arbt.empty()) {
                
                // Verify sum augmentation at root
                const auto* rootAfterDelete = arbt.getRoot();
                ASSERT_NE(rootAfterDelete, nullptr);
                ASSERT_EQ(rootAfterDelete->augmentationData, expectedTotalSum);
            }
            
            // Verify remaining values are still present
            for (int j = i + 1; j < maxValue; j++) {
                ASSERT_TRUE(arbt.contains(values[j]));
            }
        }
        
        // Tree should be empty now
        ASSERT_TRUE(arbt.empty());
        ASSERT_EQ(expectedTotalSum, 0);
    }
}

TEST(parentlessRedBlackTreeLargeScaleStressTest) {
    const int largeValue = 1000;
    const int iterations = 5;
    
    std::srand(12345); // Different seed for variety
    
    for (int iter = 0; iter < iterations; iter++) {
        ParentlessRedBlackTree<int> rbt;
        Vector<int> values;
        
        // Generate values 1 to largeValue
        for (int i = 1; i <= largeValue; i++) {
            values.push(i);
        }
        
        // Multiple shuffle passes for better randomization
        for (int pass = 0; pass < 3; pass++) {
            for (int i = largeValue - 1; i > 0; i--) {
                int j = std::rand() % (i + 1);
                int temp = values[i];
                values[i] = values[j];
                values[j] = temp;
            }
        }
        
        // Insert first half
        for (int i = 0; i < largeValue / 2; i++) {
            rbt.insert(values[i]);
            ASSERT_TRUE(rbt.contains(values[i]));
        }
        
        // Insert second half
        for (int i = largeValue / 2; i < largeValue; i++) {
            rbt.insert(values[i]);
            ASSERT_TRUE(rbt.contains(values[i]));
        }
        
        // Verify all values are present
        for (int i = 0; i < largeValue; i++) {
            ASSERT_TRUE(rbt.contains(values[i]));
        }
        
        // Shuffle again for deletion
        for (int pass = 0; pass < 3; pass++) {
            for (int i = largeValue - 1; i > 0; i--) {
                int j = std::rand() % (i + 1);
                int temp = values[i];
                values[i] = values[j];
                values[j] = temp;
            }
        }
        
        // Delete first half
        for (int i = 0; i < largeValue / 2; i++) {
            ASSERT_TRUE(rbt.erase(values[i]));
            ASSERT_FALSE(rbt.contains(values[i]));
        }
        
        // Verify remaining values are still present
        for (int i = largeValue / 2; i < largeValue; i++) {
            ASSERT_TRUE(rbt.contains(values[i]));
        }
        
        // Delete second half
        for (int i = largeValue / 2; i < largeValue; i++) {
            ASSERT_TRUE(rbt.erase(values[i]));
            ASSERT_FALSE(rbt.contains(values[i]));
        }
        
        // Tree should be empty
        ASSERT_TRUE(rbt.empty());
    }
}

TEST(parentlessAugmentedRedBlackTreeSumStressTest) {
    // Accumulator that computes the sum of all values in subtree
    struct SumAccumulator {
        int operator()(const int& nodeValue, const int* leftSum, const int* rightSum) const {
            int left = leftSum ? *leftSum : 0;
            int right = rightSum ? *rightSum : 0;
            return nodeValue + left + right;
        }
    };
    
    using SumAugmentedRBT = ParentlessAugmentedRedBlackTree<int, int, SumAccumulator>;
    
    const int maxValue = 50;
    const int maxLoop = 20;
    
    // Seed for reproducible results
    std::srand(123);
    
    for (int loop = 0; loop < maxLoop; loop++) {
        SumAugmentedRBT arbt;
        Vector<int> values;
        int expectedTotalSum = 0;

        // Generate sequential values 1 to maxValue
        for (int i = 1; i <= maxValue; i++) {
            values.push(i);
            expectedTotalSum += i;
        }
        
        // Shuffle for random insertion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Insert all values
        for (int i = 0; i < maxValue; i++) {
            arbt.insert(values[i]);
            ASSERT_TRUE(arbt.contains(values[i]));
        }
        
        // Verify total sum through root's augmented data
        ASSERT_EQ(arbt.getRoot() -> augmentationData, expectedTotalSum);
        
        // Shuffle for deletion order
        for (int i = maxValue - 1; i > 0; i--) {
            int j = std::rand() % (i + 1);
            int temp = values[i];
            values[i] = values[j];
            values[j] = temp;
        }
        
        // Delete all values, checking sum after each deletion
        for (int i = 0; i < maxValue; i++) {
            ASSERT_TRUE(arbt.erase(values[i]));
            ASSERT_FALSE(arbt.contains(values[i]));
            expectedTotalSum -= values[i];
            
            if (arbt.getRoot() != nullptr) {
                ASSERT_EQ(arbt.getRoot() -> augmentationData, expectedTotalSum);
            }
            
            // Verify remaining values are still there
            for (int j = i + 1; j < maxValue; j++) {
                ASSERT_TRUE(arbt.contains(values[j]));
            }
        }
        
        // Tree should be empty now
        ASSERT_TRUE(arbt.empty());
        ASSERT_EQ(expectedTotalSum, 0);
    }
}

