//
// Created by Spencer Martin on 8/30/25.
//

#ifndef CROCOS_LINKEDLIST_H
#define CROCOS_LINKEDLIST_H

#include <core/Iterator.h>
#include <core/ds/Optional.h>
#include <initializer_list.h>

template <typename Node, typename Extractor>
concept LinkedListNodeExtractor = requires(Node& node){
    {Extractor::previous(node)} -> convertible_to<Node*&>;
    requires IsReference<decltype(Extractor::previous(node))>;
    {Extractor::next(node)} -> convertible_to<Node*&>;
    requires IsReference<decltype(Extractor::next(node))>;
};

template <typename Node, typename Extractor>
requires LinkedListNodeExtractor<Node, Extractor>
class IntrusiveLinkedList {
    Node* headNode;
    Node* tailNode;
public:
    IntrusiveLinkedList() : headNode(nullptr), tailNode(nullptr) {}
    void pushBack(Node& node) {
        if (headNode == nullptr) {
            headNode = &node;
            tailNode = &node;
            Extractor::previous(node) = nullptr;
            Extractor::next(node) = nullptr;
        }
        else{
            Extractor::next(*tailNode) = &node;
            Extractor::previous(node) = tailNode;
            Extractor::next(node) = nullptr;
            tailNode = &node;
        }
    }

    void pushFront(Node& node) {
        if(headNode == nullptr){
            headNode = &node;
            tailNode = &node;
            Extractor::next(node) = nullptr;
            Extractor::previous(node) = nullptr;
        }
        else{
            Extractor::previous(*headNode) = &node;
            Extractor::next(node) = headNode;
            Extractor::previous(node) = nullptr;
            headNode = &node;
        }
    }

    void remove(Node& node) {
        if(headNode == &node){
            headNode = Extractor::next(*headNode);
        }
        if(tailNode == &node){
            tailNode = Extractor::previous(*tailNode);
        }
        if(Extractor::previous(node) != nullptr){
            Extractor::next(*Extractor::previous(node)) = Extractor::next(node);
        }
        if(Extractor::next(node) != nullptr){
            Extractor::previous(*Extractor::next(node)) = Extractor::previous(node);
        }
    }

    Node* head(){
        return headNode;
    }

    Node* tail(){
        return tailNode;
    }

    Node* popFront() {
        auto toReturn = headNode;
        if(headNode != nullptr){
            remove(*headNode);
        }
        return toReturn;
    }

    Node* popBack() {
        auto toReturn = tailNode;
        if(tailNode != nullptr){
            remove(*tailNode);
        }
        return toReturn;
    }

    template<bool forward>
    class NodeIterator {
        Node* current;
        //Store the next node to advance to so we can remove the current node while iterating
        Node* next;
        NodeIterator(Node* n) : current(n) {
            next = nullptr;
            if(n != nullptr){
                if constexpr(forward) {
                    next = Extractor::next(*n);
                }
                else{
                    next = Extractor::previous(*n);
                }
            }
        }
        friend class IntrusiveLinkedList;
    public:
        Node& operator*() const { return *current; }

        NodeIterator& operator++() {
            if(current == nullptr) return *this;
            current = next;
            if(next != nullptr){
                if constexpr(forward) {
                    next = Extractor::next(*next);
                }
                else{
                    next = Extractor::previous(*next);
                }
            }
            return *this;
        }

        bool operator!=(const NodeIterator& other) const {
            return current != other.current;
        }
    };

    Node* next(Node& node) {
        return Extractor::next(node);
    }

    Node* previous(Node& node) {
        return Extractor::previous(node);
    }

    NodeIterator<true> begin() const { return NodeIterator<true>(headNode); }
    NodeIterator<true> end() const { return NodeIterator<true>(nullptr); }

    IteratorRange<NodeIterator<true>> forward() const {
        return IteratorRange<NodeIterator<true>>(begin(), end());
    }

    IteratorRange<NodeIterator<false>> backward() const {
        using It = NodeIterator<false>;
        return IteratorRange<It>(It(tailNode), It(nullptr));
    }
};

template <typename NodeData>
struct StandardLinkedListNode {
    NodeData data;
    StandardLinkedListNode* previous;
    StandardLinkedListNode* next;
};

template <typename NodeData>
struct StandardLinkedListNodeExtractor {
    static StandardLinkedListNode<NodeData>*& previous(StandardLinkedListNode<NodeData>& node) {return node.previous;}
    static StandardLinkedListNode<NodeData>*& next(StandardLinkedListNode<NodeData>& node) {return node.next;}
};

template <typename NodeData>
class LinkedList : IntrusiveLinkedList<StandardLinkedListNode<NodeData>, StandardLinkedListNodeExtractor<NodeData>>{
    using Base = IntrusiveLinkedList<StandardLinkedListNode<NodeData>, StandardLinkedListNodeExtractor<NodeData>>;
    using Node = StandardLinkedListNode<NodeData>;
public:
    ~LinkedList() {
        Node* current = Base::head();
        while(current != nullptr){
            Node* nextNode = Base::next(*current);
            delete current;
            current = nextNode;
        }
    }

    LinkedList() = default;
    
    LinkedList(std::initializer_list<NodeData> init) {
        for (const auto& item : init) {
            pushBack(const_cast<NodeData&>(item));
        }
    }
    
    LinkedList(const NodeData* buffer, size_t length) {
        for (size_t i = 0; i < length; i++) {
            pushBack(const_cast<NodeData&>(buffer[i]));
        }
    }

    Node* pushBack(const NodeData& data){
        auto newNode = new Node{data, nullptr, nullptr};
        Base::pushBack(*newNode);
        return newNode;
    }

    Node* pushFront(const NodeData& data){
        auto newNode = new Node{data, nullptr, nullptr};
        Base::pushFront(*newNode);
        return newNode;
    }

    Node* pushBack(NodeData&& data){
        auto newNode = new Node{move(data), nullptr, nullptr};
        Base::pushBack(*newNode);
        return newNode;
    }

    Node* pushFront(NodeData&& data){
        auto newNode = new Node{move(data), nullptr, nullptr};
        Base::pushFront(*newNode);
        return newNode;
    }

    void remove(Node*& node) {
        Base::remove(*node);
        delete node;
        node = nullptr;
    }

    Optional<NodeData> popFront() {
        auto front = Base::popFront();
        Optional<NodeData> toReturn = {};
        if(front != nullptr){
            toReturn = front->data;
            delete front;
        }
        return toReturn;
    }

    Optional<NodeData> popBack() {
        auto back = Base::popBack();
        Optional<NodeData> toReturn = {};
        if(back != nullptr){
            toReturn = back->data;
            delete back;
        }
        return toReturn;
    }

    template <bool Forward, typename Itr = typename Base::template NodeIterator<Forward>>
    class ValueIterator {
        Itr current;
        friend class LinkedList;
        ValueIterator(Itr n) : current(n) {}
    public:
        NodeData& operator*() const { return (*current).data; }
        ValueIterator& operator++() {
            ++current;
            return *this;
        }
        bool operator!=(const ValueIterator& other) const {
            return current != other.current;
        }
    };

    ValueIterator<true> begin() const { return ValueIterator<true>(Base::begin()); }

    ValueIterator<true> end() const { return ValueIterator<true>(Base::end()); }

    IteratorRange<ValueIterator<true>> forward() const {
        return IteratorRange<ValueIterator<true>>(begin(), end());
    }

    IteratorRange<ValueIterator<false>> backward() const {
        auto it = Base::backward();
        return IteratorRange<ValueIterator<false>>(it.begin(), it.end());
    }

    auto forwardNodes() const {
        return Base::forward();
    }

    auto backwardNodes() const {
        return Base::backward();
    }

    NodeData* head(){
        if(Base::head() != nullptr){
            return &Base::head()->data;
        }
        return nullptr;
    }

    NodeData* tail(){
        if(Base::tail() != nullptr){
            return &Base::tail()->data;
        }
        return nullptr;
    }

    bool empty(){
        return Base::head() == nullptr;
    }

    Node* headNode() {
        return Base::head();
    }

    Node* tailNode() {
        return Base::tail();
    }

    using Base::next;
    using Base::previous;
};

#endif //CROCOS_LINKEDLIST_H