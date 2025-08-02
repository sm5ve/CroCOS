//
// Created by Spencer Martin on 8/1/25.
//

#ifndef TREES_H
#define TREES_H

#include <core/utility.h>
#include <core/Comparator.h>

template <typename NodeType, typename BinaryTreeInfoExtractor>
concept BinaryTreeNodeType = requires(NodeType& node){
    {BinaryTreeInfoExtractor::left(node)} -> IsSame<NodeType*&>;
    {BinaryTreeInfoExtractor::right(node)} -> IsSame<NodeType*&>;
    requires IsReference<decltype(BinaryTreeInfoExtractor::data(node))>;
};

template <typename NodeType, typename RedBlackTreeInfoExtractor>
concept RedBlackTreeNodeType = requires(NodeType& node, const RedBlackTreeInfoExtractor extractor, bool b){
    {RedBlackTreeInfoExtractor::isRed(node)} -> convertible_to<bool>;
    {RedBlackTreeInfoExtractor::setRed(node, b)} -> IsSame<void>;
    BinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;
};

enum TreeSearchAction{
    Continue,
    Stop
};

template <typename NodeType, typename Visitor>
concept TreeVisitor = requires(NodeType& node, Visitor&& visitor){
    {visitor(node)} -> IsSame<TreeSearchAction>;
};

template <typename NodeType, typename Visitor>
concept ConstTreeVisitor = requires(const NodeType& node, Visitor&& visitor){
    {visitor(node)} -> IsSame<TreeSearchAction>;
};

template <typename NodeType, typename BinaryTreeInfoExtractor>
requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>
class IntrusiveBinaryTree{
protected:
    template <typename Visitor>
    class VisitorWrapper{
        Visitor&& visitor;
    public:
        VisitorWrapper(Visitor&& visitor) : visitor(forward<Visitor>(visitor)){}

        TreeSearchAction operator()(NodeType& node){
            if constexpr(is_same_v<decltype(visitor(node)), void>){
                visitor(node);
                return Continue;
            }
            else{
                static_assert(is_same_v<decltype(visitor(node)), TreeSearchAction>);
                return visitor(node);
            }
        }
    };

    template <typename Visitor>
    class ConstVisitorWrapper{
        Visitor&& visitor;
    public:
        ConstVisitorWrapper(Visitor&& visitor) : visitor(forward<Visitor>(visitor)){}

        TreeSearchAction operator()(const NodeType& node){
            if constexpr(is_same_v<decltype(visitor(node)), void>){
                visitor(node);
                return Continue;
            }
            else{
                static_assert(is_same_v<decltype(visitor(node)), TreeSearchAction>);
                return visitor(node);
            }
        }
    };

    NodeType* root;

    template <typename Visitor>
    requires TreeVisitor<NodeType, Visitor>
    TreeSearchAction visitDepthFirstInOrderImpl(Visitor&& visitor, NodeType* node = nullptr){
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitor(*node);
        if(result == Stop){
            return Stop;
        }
        return visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
    }

    template <typename Visitor>
    requires TreeVisitor<NodeType, Visitor>
    TreeSearchAction visitDepthFirstReverseOrderImpl(Visitor&& visitor, NodeType* node = nullptr){
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitor(*node);
        if(result == Stop){
            return Stop;
        }
        return visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
    }

    template <typename Visitor>
    requires TreeVisitor<NodeType, Visitor>
    TreeSearchAction visitDepthFirstPostOrderImpl(Visitor&& visitor, NodeType* node = nullptr){
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
        if(result == Stop){
            return Stop;
        }
        return visitor(*node);
    }

    template <typename Visitor>
        requires ConstTreeVisitor<NodeType, Visitor>
        TreeSearchAction visitDepthFirstInOrderImpl(Visitor&& visitor, const NodeType* node = nullptr) const{
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitor(*node);
        if(result == Stop){
            return Stop;
        }
        return visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
    }

    template <typename Visitor>
    requires ConstTreeVisitor<NodeType, Visitor>
    TreeSearchAction visitDepthFirstReverseOrderImpl(Visitor&& visitor, const NodeType* node = nullptr) const{
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitor(*node);
        if(result == Stop){
            return Stop;
        }
        return visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
    }

    template <typename Visitor>
    requires ConstTreeVisitor<NodeType, Visitor>
    TreeSearchAction visitDepthFirstPostOrderImpl(Visitor&& visitor, const NodeType* node = nullptr) const{
        if(node == nullptr){
            return Continue;
        }
        auto result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
        if(result == Stop){
            return Stop;
        }
        result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
        if(result == Stop){
            return Stop;
        }
        return visitor(*node);
    }

    bool rotateLeft(NodeType*& node){
        if(node == nullptr){
            return false;
        }
        if(BinaryTreeInfoExtractor::right(node) == nullptr){
            return false;
        }
        NodeType* pivot = node;
        NodeType* newRoot = BinaryTreeInfoExtractor::right(node);
        //Pivot's right child becomes newRoot's left child
        BinaryTreeInfoExtractor::right(*pivot) = BinaryTreeInfoExtractor::left(*newRoot);
        //NewRoot's left child becomes pivot
        BinaryTreeInfoExtractor::left(*newRoot) = pivot;
        //Update the root.
        node = newRoot;
        return true;
    }

    bool rotateRight(NodeType*& node){
        if(node == nullptr){
            return false;
        }
        if(BinaryTreeInfoExtractor::left(node) == nullptr){
            return false;
        }
        NodeType* pivot = node;
        NodeType* newRoot = BinaryTreeInfoExtractor::left(node);
        //Pivot's left child becomes newRoot's right child
        BinaryTreeInfoExtractor::left(*pivot) = BinaryTreeInfoExtractor::right(*newRoot);
        //NewRoot's right child becomes pivot
        BinaryTreeInfoExtractor::right(*newRoot) = pivot;
        //Update the root
        node = newRoot;
        return true;
    }
public:
    IntrusiveBinaryTree() : root(nullptr){}
    explicit IntrusiveBinaryTree(NodeType* r) : root(r){}

    template <typename Visitor>
    void visitDepthFirstInOrder(Visitor&& visitor){
        visitDepthFirstInOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstInOrder(Visitor&& visitor, NodeType* start){
        visitDepthFirstInOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
    }

    template <typename Visitor>
    void visitDepthFirstReverseOrder(Visitor&& visitor){
        visitDepthFirstReverseOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstReverseOrder(Visitor&& visitor, NodeType* start){
        visitDepthFirstReverseOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
    }

    template <typename Visitor>
    void visitDepthFirstPostOrder(Visitor&& visitor){
        visitDepthFirstPostOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstPostOrder(Visitor&& visitor, NodeType* start){
        visitDepthFirstPostOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
    }

    template <typename Visitor>
    void visitDepthFirstInOrder(Visitor&& visitor) const{
        visitDepthFirstInOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstInOrder(Visitor&& visitor, const NodeType* start) const{
        visitDepthFirstInOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
    }

    template <typename Visitor>
    void visitDepthFirstReverseOrder(Visitor&& visitor) const{
        visitDepthFirstReverseOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstReverseOrder(Visitor&& visitor, const NodeType* start) const{
        visitDepthFirstReverseOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
    }

    template <typename Visitor>
    void visitDepthFirstPostOrder(Visitor&& visitor) const{
        visitDepthFirstPostOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
    }

    template <typename Visitor>
    void visitDepthFirstPostOrder(Visitor&& visitor, const NodeType* start) const{
        visitDepthFirstPostOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
    }

    //Intentionally does not delete an overwritten child, since this is intrusive and meant to support use in LibAlloc
    void setLeftChild(NodeType* parent, NodeType* child){
        if(parent != nullptr){
            BinaryTreeInfoExtractor::left(*parent) = child;
        }
    }

    void setRightChild(NodeType* parent, NodeType* child){
        if(parent != nullptr){
            BinaryTreeInfoExtractor::right(*parent) = child;
        }
    }
};

template <typename NodeType, typename BinaryTreeInfoExtractor>
requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>
using DefaultBinaryTreeNodeComparator = DefaultComparator<remove_reference_t<decltype(BinaryTreeInfoExtractor::data(std::declval<NodeType&>()))>>;

template <typename NodeType, typename BinaryTreeInfoExtractor, typename Comparator>
concept BinaryTreeComparator = requires(const NodeType& node1, const NodeType& node2, Comparator& comparator){
    {comparator(BinaryTreeInfoExtractor::data(node1), BinaryTreeInfoExtractor::data(node2))} -> convertible_to<bool>;
};

template <typename NodeType, typename BinaryTreeInfoExtractor, typename Comparator=DefaultBinaryTreeNodeComparator<NodeType, BinaryTreeInfoExtractor>>
requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor> && BinaryTreeComparator<NodeType, BinaryTreeInfoExtractor, Comparator>
class IntrusiveBinarySearchTree : protected IntrusiveBinaryTree<NodeType, BinaryTreeInfoExtractor>{
    using Parent = IntrusiveBinaryTree<NodeType, BinaryTreeInfoExtractor>;
    using NodeData = decltype(BinaryTreeInfoExtractor::data(std::declval<NodeType&>()));

protected:
    Comparator comparator;

    void insert(NodeType* toInsert, NodeType*& root){
        NodeType** current = &root;

        while(*current != nullptr){
            if(comparator(BinaryTreeInfoExtractor::data(*toInsert), BinaryTreeInfoExtractor::data(**current))){
                current = &BinaryTreeInfoExtractor::left(**current);
            }
            else{
                current = &BinaryTreeInfoExtractor::right(**current);
            }
        }
        *current = toInsert;

        //Make sure to set the children to null just in case there's any stale data in the node
        BinaryTreeInfoExtractor::left(*toInsert) = nullptr;
        BinaryTreeInfoExtractor::right(*toInsert) = nullptr;
    }

    NodeType* findImpl(const remove_reference_t<NodeData>& value, NodeType* root) const{
        NodeType* current = root;
        while(current != nullptr){
            if(value == BinaryTreeInfoExtractor::data(*current)){
                return current;
            }
            //if value < current, go left
            if(comparator(value, BinaryTreeInfoExtractor::data(*current))){
                current = BinaryTreeInfoExtractor::left(*current);
            }
            //otherwise go right
            else{
                current = BinaryTreeInfoExtractor::right(*current);
            }
        }
        return nullptr;
    }
    
    const NodeType* findImpl(const remove_reference_t<NodeData>& value, const NodeType* root) const{
        const NodeType* current = root;
        while(current != nullptr){
            if(value == BinaryTreeInfoExtractor::data(*current)){
                return current;
            }
            //if value < current, go left
            if(comparator(value, BinaryTreeInfoExtractor::data(*current))){
                current = BinaryTreeInfoExtractor::left(*current);
            }
            //otherwise go right
            else{
                current = BinaryTreeInfoExtractor::right(*current);
            }
        }
        return nullptr;
    }

    NodeType* eraseNodeImpl(NodeType** toRemove){
        NodeType* toReturn = *toRemove;
        //If one of the children of the node we are trying to erase is null, deletion is simple: just replace
        //the pointer to toRemove in its parent with the other child
        if(BinaryTreeInfoExtractor::left(*(*toRemove)) == nullptr){
            *toRemove = BinaryTreeInfoExtractor::right(*(*toRemove));
        }
        else if(BinaryTreeInfoExtractor::right(*(*toRemove)) == nullptr){
            *toRemove = BinaryTreeInfoExtractor::left(*(*toRemove));
        }
        //Otherwise, we need to find the successor of toRemove, and replace it with the successor
        else{
            //Find the successor of toRemove
            NodeType** successor = &BinaryTreeInfoExtractor::right(*(*toRemove));
            while(BinaryTreeInfoExtractor::left(*(*successor)) != nullptr){
                successor = &BinaryTreeInfoExtractor::left(*(*successor));
            }

            //Retain a pointer to the right child of the successor
            NodeType* successorPtr = *successor;
            //Remove the successor from the tree, replace it with its right child. The left will always be empty
            *successor = BinaryTreeInfoExtractor::right(*successorPtr);

            //Now copy the child pointers from toRemove to the successor
            BinaryTreeInfoExtractor::left(*successorPtr) = BinaryTreeInfoExtractor::left(*(*toRemove));
            BinaryTreeInfoExtractor::right(*successorPtr) = BinaryTreeInfoExtractor::right(*(*toRemove));

            //update the parent pointer toRemove to point to its successor
            *toRemove = successorPtr;
        }
        return toReturn;
    }

    //Returns a pointer to the erased type - in an intrusive environment, it is up to the caller to delete the memory
    //as appropriate
    NodeType* eraseImpl(const remove_reference_t<NodeData>& value, NodeType*& root){
        NodeType** current = &root;
        while(*current != nullptr){
            if(BinaryTreeInfoExtractor::data(*(*current)) == value){
                //We found the thing to erase, so let's erase it
                return eraseNodeImpl(current);
            }
            //If value < current, go left
            else if(comparator(value, BinaryTreeInfoExtractor::data(*(*current)))){
                current = &BinaryTreeInfoExtractor::left(**current);
            }
            //Otherwise go right
            else{
                current = &BinaryTreeInfoExtractor::right(**current);
            }
        }
        return nullptr;
    }

    //Returns a pointer to the erased type - in an intrusive environment, it is up to the caller to delete the memory
    //as appropriate
    NodeType* eraseImpl(const NodeType* value, NodeType*& root){
        NodeType** current = &root;
        while(*current != nullptr){
            if(value == *current){
                return eraseNodeImpl(current);
            }
            //If value < current, go left
            else if(comparator(BinaryTreeInfoExtractor::data(*value), BinaryTreeInfoExtractor::data(*(*current)))){
                current = &BinaryTreeInfoExtractor::left(**current);
            }
            //Otherwise go right
            else{
                current = &BinaryTreeInfoExtractor::right(**current);
            }
        }
        return nullptr;
    }
public:
    using Parent::visitDepthFirstInOrder;
    using Parent::visitDepthFirstReverseOrder;
    using Parent::visitDepthFirstPostOrder;

    void insert(NodeType* node){
        this -> insert(node, this -> root);
    }

    NodeType* find(const remove_reference_t<NodeData>& value){
        return this -> findImpl(value, this -> root);
    }
    
    const NodeType* find(const remove_reference_t<NodeData>& value) const{
        return this -> findImpl(value, this -> root);
    }

    NodeType* erase(const remove_reference_t<NodeData>& value){
        return this -> eraseImpl(value, this -> root);
    }

    NodeType* erase(NodeType* value){
        return this -> eraseImpl(value, this -> root);
    }

    NodeType* floor(const remove_reference_t<NodeData>& value) const{
        NodeType* result = nullptr;
        NodeType* current = this -> root;

        while(current != nullptr){
            if(BinaryTreeInfoExtractor::data(*current) == value){
                return current; // Exact match
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*current), value)){
                // current < value, so current is a candidate
                result = current;
                current = BinaryTreeInfoExtractor::right(*current);
            }
            else{
                // current > value, go left
                current = BinaryTreeInfoExtractor::left(*current);
            }
        }
        return result;
    }

    NodeType* ceil(const remove_reference_t<NodeData>& value) const{
        NodeType* result = nullptr;
        NodeType* current = this -> root;

        while(current != nullptr){
            if(BinaryTreeInfoExtractor::data(*current) == value){
                return current; // Exact match
            }
            else if(comparator(value, BinaryTreeInfoExtractor::data(*current))){
                // value < current, so current is a candidate
                result = current;
                current = BinaryTreeInfoExtractor::left(*current);
            }
            else{
                // value > current, go right
                current = BinaryTreeInfoExtractor::right(*current);
            }
        }
        return result;
    }

    NodeType* successor(const NodeType* node) const{
        if(node == nullptr) return nullptr;
        
        // Case 1: node has right child
        if(BinaryTreeInfoExtractor::right(*node) != nullptr){
            NodeType* current = BinaryTreeInfoExtractor::right(*node);
            while(BinaryTreeInfoExtractor::left(*current) != nullptr){
                current = BinaryTreeInfoExtractor::left(*current);
            }
            return current;
        }
        
        // Case 2: no right child, find ancestor where node is in left subtree
        NodeType* successor = nullptr;
        NodeType* current = this -> root;
        
        while(current != nullptr){
            if(comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*current))){
                successor = current;  // current > node, so it's a candidate
                current = BinaryTreeInfoExtractor::left(*current);
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*current), BinaryTreeInfoExtractor::data(*node))){
                current = BinaryTreeInfoExtractor::right(*current);
            }
            else{
                // Found the node, successor is already set (or null)
                break;
            }
        }
        return successor;
    }

    NodeType* predecessor(const NodeType* node) const{
        if(node == nullptr) return nullptr;
        
        // Case 1: node has left child
        if(BinaryTreeInfoExtractor::left(*node) != nullptr){
            NodeType* current = BinaryTreeInfoExtractor::left(*node);
            while(BinaryTreeInfoExtractor::right(*current) != nullptr){
                current = BinaryTreeInfoExtractor::right(*current);
            }
            return current;
        }
        
        // Case 2: no left child, find ancestor where node is in right subtree
        NodeType* predecessor = nullptr;
        NodeType* current = this -> root;
        
        while(current != nullptr){
            if(comparator(BinaryTreeInfoExtractor::data(*current), BinaryTreeInfoExtractor::data(*node))){
                predecessor = current;  // current < node, so it's a candidate
                current = BinaryTreeInfoExtractor::right(*current);
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*current))){
                current = BinaryTreeInfoExtractor::left(*current);
            }
            else{
                // Found the node, predecessor is already set (or null)
                break;
            }
        }
        return predecessor;
    }
};

// Value-owning tree node for standard (non-intrusive) trees
template<typename T>
struct TreeNode {
    T data;
    TreeNode* left;
    TreeNode* right;
    
    TreeNode(const T& value) : data(value), left(nullptr), right(nullptr) {}
    TreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr) {}
};

// Extractor for TreeNode
template<typename T>
struct TreeNodeExtractor {
    static TreeNode<T>*& left(TreeNode<T>& node) { return node.left; }
    static TreeNode<T>*& right(TreeNode<T>& node) { return node.right; }
    static TreeNode<T>* const& left(const TreeNode<T>& node) { return node.left; }
    static TreeNode<T>* const& right(const TreeNode<T>& node) { return node.right; }
    static T& data(TreeNode<T>& node) { return node.data; }
    static const T& data(const TreeNode<T>& node) { return node.data; }
};

// Value-owning Binary Tree
template<typename T>
class BinaryTree : private IntrusiveBinaryTree<TreeNode<T>, TreeNodeExtractor<T>> {
    using Node = TreeNode<T>;
    using Parent = IntrusiveBinaryTree<Node, TreeNodeExtractor<T>>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    BinaryTree() = default;
    
    // Constructor with root value
    explicit BinaryTree(const T& rootValue) {
        this->root = new Node(rootValue);
    }
    
    explicit BinaryTree(T&& rootValue) {
        this->root = new Node(move(rootValue));
    }
    
    ~BinaryTree() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment - trees are move-only for simplicity
    BinaryTree(const BinaryTree&) = delete;
    BinaryTree& operator=(const BinaryTree&) = delete;
    
    // Move constructor and assignment
    BinaryTree(BinaryTree&& other) noexcept : Parent(other.root) {
        other.root = nullptr;
    }
    
    BinaryTree& operator=(BinaryTree&& other) noexcept {
        if (this != &other) {
            deleteSubtree(this->root);
            this->root = other.root;
            other.root = nullptr;
        }
        return *this;
    }
    
    // Set root (replaces existing tree)
    void setRoot(const T& value) {
        deleteSubtree(this->root);
        this->root = new Node(value);
    }
    
    void setRoot(T&& value) {
        deleteSubtree(this->root);
        this->root = new Node(move(value));
    }
    
    // Manual tree building methods
    void setLeftChild(Node* parent, const T& value) {
        if (parent != nullptr && parent->left == nullptr) {
            parent->left = new Node(value);
        }
    }
    
    void setRightChild(Node* parent, const T& value) {
        if (parent != nullptr && parent->right == nullptr) {
            parent->right = new Node(value);
        }
    }
    
    Node* getRoot() { return this->root; }
    const Node* getRoot() const { return this->root; }
    
    // Expose visitor methods
    using Parent::visitDepthFirstInOrder;
    using Parent::visitDepthFirstReverseOrder;
    using Parent::visitDepthFirstPostOrder;
    
    bool empty() const { return this->root == nullptr; }
};

// Value-owning Binary Search Tree
template<typename T, typename Comparator = DefaultComparator<T>>
class BinarySearchTree : private IntrusiveBinarySearchTree<TreeNode<T>, TreeNodeExtractor<T>, Comparator> {
    using Node = TreeNode<T>;
    using Parent = IntrusiveBinarySearchTree<Node, TreeNodeExtractor<T>, Comparator>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    BinarySearchTree() = default;
    explicit BinarySearchTree(Comparator comp) : Parent() {
        this->comparator = comp;
    }
    
    ~BinarySearchTree() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment
    BinarySearchTree(const BinarySearchTree&) = delete;
    BinarySearchTree& operator=(const BinarySearchTree&) = delete;
    
    // Move constructor and assignment
    BinarySearchTree(BinarySearchTree&& other) noexcept : Parent() {
        this->root = other.root;
        this->comparator = move(other.comparator);
        other.root = nullptr;
    }
    
    BinarySearchTree& operator=(BinarySearchTree&& other) noexcept {
        if (this != &other) {
            deleteSubtree(this->root);
            this->root = other.root;
            this->comparator = move(other.comparator);
            other.root = nullptr;
        }
        return *this;
    }
    
    // Insert operations
    void insert(const T& value) {
        Node* node = new Node(value);
        Parent::insert(node);
    }
    
    void insert(T&& value) {
        Node* node = new Node(move(value));
        Parent::insert(node);
    }
    
    // Find operation
    bool contains(const T& value) const {
        return Parent::find(value) != nullptr;
    }
    
    // Erase operation
    bool erase(const T& value) {
        Node* node = Parent::erase(value);
        if (node != nullptr) {
            delete node;
            return true;
        }
        return false;
    }
    
    // Floor operation - largest element <= value
    bool floor(const T& value, T& result) const {
        Node* node = Parent::floor(value);
        if (node != nullptr) {
            result = node->data;
            return true;
        }
        return false;
    }
    
    // Ceil operation - smallest element >= value  
    bool ceil(const T& value, T& result) const {
        Node* node = Parent::ceil(value);
        if (node != nullptr) {
            result = node->data;
            return true;
        }
        return false;
    }
    
    // Successor operation - next larger element
    bool successor(const T& value, T& result) const {
        const Node* node = Parent::find(value);
        if (node != nullptr) {
            Node* succ = Parent::successor(node);
            if (succ != nullptr) {
                result = succ->data;
                return true;
            }
        }
        return false;
    }
    
    // Predecessor operation - next smaller element
    bool predecessor(const T& value, T& result) const {
        const Node* node = Parent::find(value);
        if (node != nullptr) {
            Node* pred = Parent::predecessor(node);
            if (pred != nullptr) {
                result = pred->data;
                return true;
            }
        }
        return false;
    }
    
    // Expose visitor methods
    using Parent::visitDepthFirstInOrder;
    using Parent::visitDepthFirstReverseOrder;
    using Parent::visitDepthFirstPostOrder;
    
    bool empty() const { return this->root == nullptr; }
};

#endif //TREES_H
