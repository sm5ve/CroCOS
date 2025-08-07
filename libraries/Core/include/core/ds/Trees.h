//
// Created by Spencer Martin on 8/1/25.
//

#ifndef TREES_H
#define TREES_H

#include <core/utility.h>
#include <core/Comparator.h>
#include <core/ds/Tuple.h>
#include <core/ds/Optional.h>
#include <core/ds/Stack.h>
#include <core/PrintStream.h>
#include <assert.h>

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
        if(BinaryTreeInfoExtractor::right(*node) == nullptr){
            return false;
        }
        NodeType* pivot = node;
        NodeType* newRoot = BinaryTreeInfoExtractor::right(*node);
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
        if(BinaryTreeInfoExtractor::left(*node) == nullptr){
            return false;
        }
        NodeType* pivot = node;
        NodeType* newRoot = BinaryTreeInfoExtractor::left(*node);
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

    NodeType* const& floorImpl(const remove_reference_t<NodeData>& value) const{
        static NodeType* const invalid_node = nullptr;
        NodeType*const * result = &invalid_node;
        NodeType*const * current = &(this -> root);

        while(*current != nullptr){
            if(BinaryTreeInfoExtractor::data(*(*current)) == value){
                return *current; // Exact match
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*(*current)), value)){
                // current < value, so current is a candidate
                result = current;
                current = &BinaryTreeInfoExtractor::right(*(*current));
            }
            else{
                // current > value, go left
                current = &BinaryTreeInfoExtractor::left(*(*current));
            }
        }
        return *result;
    }

    NodeType* const& ceilImpl(const remove_reference_t<NodeData>& value) const{
        static NodeType* const invalid_node = nullptr;
        NodeType* const* result = &invalid_node;
        NodeType* const* current = &(this -> root);

        while(*current != nullptr){
            if(BinaryTreeInfoExtractor::data(*(*current)) == value){
                return *current; // Exact match
            }
            else if(comparator(value, BinaryTreeInfoExtractor::data(*(*current)))){
                // value < current, so current is a candidate
                result = current;
                current = &BinaryTreeInfoExtractor::left(*(*current));
            }
            else{
                // value > current, go right
                current = &BinaryTreeInfoExtractor::right(*(*current));
            }
        }
        return *result;
    }

    NodeType* const& successorImpl(const NodeType* node) const{
        static NodeType* const invalid_node = nullptr;
        if(node == nullptr) return invalid_node;

        // Case 1: node has right child
        if(BinaryTreeInfoExtractor::right(*node) != nullptr){
            NodeType*const* current = &BinaryTreeInfoExtractor::right(*node);
            while(BinaryTreeInfoExtractor::left(*(*current)) != nullptr){
                current = &BinaryTreeInfoExtractor::left(*(*current));
            }
            return *current;
        }

        // Case 2: no right child, find ancestor where node is in left subtree
        NodeType* const* successor = &invalid_node;
        NodeType* const* current = &(this -> root);

        while(*current != nullptr){
            if(comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*(*current)))){
                successor = current;  // current > node, so it's a candidate
                current = &BinaryTreeInfoExtractor::left(*(*current));
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*(*current)), BinaryTreeInfoExtractor::data(*node))){
                current = &BinaryTreeInfoExtractor::right(*(*current));
            }
            else{
                // Found the node, successor is already set (or null)
                break;
            }
        }
        return *successor;
    }

    NodeType* const& predecessorImpl(const NodeType* node) const{
        static NodeType* const invalid_node = nullptr;
        if(node == nullptr) return invalid_node;

        // Case 1: node has left child
        if(BinaryTreeInfoExtractor::left(*node) != nullptr){
            NodeType*const* current = &BinaryTreeInfoExtractor::left(*node);
            while(BinaryTreeInfoExtractor::right(*(*current)) != nullptr){
                current = &BinaryTreeInfoExtractor::right(*(*current));
            }
            return *current;
        }

        // Case 2: no left child, find ancestor where node is in right subtree
        NodeType* const* predecessor = &invalid_node;
        NodeType* const* current = &(this -> root);

        while(*current != nullptr){
            if(comparator(BinaryTreeInfoExtractor::data(*(*current)), BinaryTreeInfoExtractor::data(*node))){
                predecessor = current;  // current < node, so it's a candidate
                current = &BinaryTreeInfoExtractor::right(*(*current));
            }
            else if(comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*(*current)))){
                current = &BinaryTreeInfoExtractor::left(*(*current));
            }
            else{
                // Found the node, predecessor is already set (or null)
                break;
            }
        }
        return *predecessor;
    }

    //Kinda EVIL approach, but this seems the best way to reuse the above implementations for both the public-facing
    //const queries and the internal-facing methods for use in the red-black tree implementation

    NodeType*& unsafeInternalSuccessor(NodeType* node){
        const NodeType* const& cref = successorImpl(*node);
        return *const_cast<NodeType**>(&cref);
    }

    NodeType*& unsafeInternalPredecessor(NodeType* node){
        const NodeType* const& cref = predecessorImpl(*node);
        return *const_cast<NodeType**>(&cref);
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

    const NodeType* floor(const remove_reference_t<NodeData>& value) const{
        return this -> floorImpl(value);
    }

    const NodeType* ceil(const remove_reference_t<NodeData>& value) const{
        return this -> ceilImpl(value);
    }

    const NodeType* successor(const NodeType* node) const{
        return this -> successorImpl(node);
    }

    const NodeType* predecessor(const NodeType* node) const{
        return this -> predecessorImpl(node);
    }
};

template <typename NodeType, typename RedBlackTreeInfoExtractor, typename Comparator>
requires BinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor> &&
    BinaryTreeComparator<NodeType, RedBlackTreeInfoExtractor, Comparator> &&
    RedBlackTreeNodeType<NodeType, RedBlackTreeInfoExtractor>
class IntrusiveRedBlackTree : protected IntrusiveBinarySearchTree<NodeType, RedBlackTreeInfoExtractor, Comparator>{
protected:
using BSTParent = IntrusiveBinarySearchTree<NodeType, RedBlackTreeInfoExtractor, Comparator>;
using BinTreeParent = IntrusiveBinaryTree<NodeType, RedBlackTreeInfoExtractor>;
Comparator comparator;
using NodeData = remove_reference_t<decltype(RedBlackTreeInfoExtractor::data(std::declval<NodeType&>()))>;

public:
void dumpAsDot(Core::PrintStream& stream) requires Core::Printable<NodeData>{
	stream << "digraph G {\n";
	stream << "rankdir=TB;\n";
	stream << "node [fontname=\"Helvetica\", shape=circle, style=filled];\n";
	this -> visitDepthFirstInOrder([&stream](NodeType& node){
		stream << "v" << reinterpret_cast<uint64_t>(&node) << "[fillcolor=";
		stream << (!RedBlackTreeInfoExtractor::isRed(node) ? "black" : "red") << ", ";
		stream << "label=" << RedBlackTreeInfoExtractor::data(node);
		if(!RedBlackTreeInfoExtractor::isRed(node)){
		stream << ", fontcolor=white";
		}
		stream << "];\n";
	});
	uint64_t nullCount = 0;
	this -> visitDepthFirstInOrder([&stream, &nullCount](NodeType& node){
		if(RedBlackTreeInfoExtractor::left(node) != nullptr){
			stream << "v" << reinterpret_cast<uint64_t>(&node) <<
					" -> v" << reinterpret_cast<uint64_t>(RedBlackTreeInfoExtractor::left(node));
			stream << " [label=\"L\"];\n";
		}
		else{
			nullCount++;
			stream << "null" << nullCount << "[label=\"\", shape=point];\n";
			stream << "v" << reinterpret_cast<uint64_t>(&node) <<
					" -> null" << nullCount << " [label=\"L\"];\n";
		}
		if(RedBlackTreeInfoExtractor::right(node) != nullptr){
			stream << "v" << reinterpret_cast<uint64_t>(&node) <<
					" -> v" << reinterpret_cast<uint64_t>(RedBlackTreeInfoExtractor::right(node));
			stream << " [label=\"R\"];\n";
		}
		else{
			nullCount++;
			stream << "null" << nullCount << "[label=\"\", shape=point];\n";
			stream << "v" << reinterpret_cast<uint64_t>(&node) <<
					" -> null" << nullCount << " [label=\"R\"];\n";
		}
	});
	stream << "}\n";
}

protected:

enum Direction{
    Left,
    Right
};

Direction opposite(Direction direction){
    if(direction == Direction::Left){
        return Direction::Right;
    }
    return Direction::Left;
}

NodeType*& getChild(NodeType& node, Direction direction){
	if(direction == Direction::Left){
		return RedBlackTreeInfoExtractor::left(node);
	}
	return RedBlackTreeInfoExtractor::right(node);
}

Direction getChildDirection(NodeType& parent, NodeType* childPtr){
    if(RedBlackTreeInfoExtractor::left(parent) == childPtr){
        return Direction::Left;
    }
    return Direction::Right;
}

bool isChild(const NodeType& parent, const NodeType* childPtr){
    return RedBlackTreeInfoExtractor::left(parent) == childPtr || RedBlackTreeInfoExtractor::right(parent) == childPtr;
}

void rotateSubtree(NodeType*& root, Direction direction){
    if(direction == Direction::Left){
        BinTreeParent::rotateLeft(root);
    }
    else{
        BinTreeParent::rotateRight(root);
    }
}

template <typename StackType>
void rotateAboutParent(StackType& ancestryStack, Direction direction){
	NodeType** current = ancestryStack[-1];
	NodeType** parent = ancestryStack[-2];
	bool rotatingTowardsCurrent = (getChild(**parent, direction) == *current);
	//Perform the rotation
	rotateSubtree(*parent, direction);

	ancestryStack.pop();
	//If we're rotating towards current, its depth will go down
	if(rotatingTowardsCurrent){
		ancestryStack.push(&getChild(**parent, direction));
		ancestryStack.push(current);
	}
}

enum Color{
    Red,
    Black
};

Color getColor(NodeType& node){
    return RedBlackTreeInfoExtractor::isRed(node) ? Color::Red : Color::Black;
}

void setColor(NodeType& node, Color color){
    RedBlackTreeInfoExtractor::setRed(node, color == Color::Red);
}

bool hasLeftChild(NodeType& node){
    return RedBlackTreeInfoExtractor::left(node) != nullptr;
}

bool hasRightChild(NodeType& node){
    return RedBlackTreeInfoExtractor::right(node) != nullptr;
}

bool hasChild(NodeType& node){
    return hasLeftChild(node) || hasRightChild(node);
}

bool verifyRedBlackTree(NodeType* node, size_t& blackHeight){
	if(node == nullptr){
		blackHeight = 1;
		return true;
	}

	if(getColor(*node) == Color::Red){
		if(hasLeftChild(*node) && getColor(*RedBlackTreeInfoExtractor::left(*node)) == Color::Red){
			assert(false, "Red violation");
			return false;
		}
		if(hasRightChild(*node) && getColor(*RedBlackTreeInfoExtractor::right(*node)) == Color::Red){
			assert(false, "Red violation");
			return false;
		}
	}

	size_t leftBlackHeight = 0;
	size_t rightBlackHeight = 0;

	if(!verifyRedBlackTree(RedBlackTreeInfoExtractor::left(*node), leftBlackHeight) || !verifyRedBlackTree(RedBlackTreeInfoExtractor::right(*node), rightBlackHeight)){
		return false;
	}

	if(leftBlackHeight != rightBlackHeight){
		assert(false, "Black violation");
		return false;
	}

	blackHeight = (getColor(*node) == Color::Black) ? (leftBlackHeight + 1) : leftBlackHeight;
	return true;
}

void verifyRedBlackTree(){
	size_t blackHeight = 0;
	assert(verifyRedBlackTree(this -> root, blackHeight), "RBT verification failed");
}

template <typename StackType>
bool verifyAlmostRedBlackTreeImpl(NodeType*& node, size_t& blackHeight, const StackType& ancestryStack, size_t& nodeCount){

	size_t virtualBlackHeight = (&node == ancestryStack[-1] ? 2 : 1);
	size_t virtualNodeCount = (node == nullptr ? 0 : 1) + (&node == ancestryStack[-1] ? 1 : 0);

	if(node == nullptr){
		blackHeight = virtualBlackHeight;
		nodeCount = virtualNodeCount;
		return true;
	}

	if(hasLeftChild(*node)){
		assert(comparator(RedBlackTreeInfoExtractor::data(*RedBlackTreeInfoExtractor::left(*node)), RedBlackTreeInfoExtractor::data(*node)), "Left child is not less than parent");
	}

	if(hasRightChild(*node)){
		assert(comparator(RedBlackTreeInfoExtractor::data(*node), RedBlackTreeInfoExtractor::data(*RedBlackTreeInfoExtractor::right(*node))), "Right child is not greater than parent");
	}

	if(getColor(*node) == Color::Red){
		if(hasLeftChild(*node) && getColor(*RedBlackTreeInfoExtractor::left(*node)) == Color::Red){
			return false;
		}
		if(hasRightChild(*node) && getColor(*RedBlackTreeInfoExtractor::right(*node)) == Color::Red){
			return false;
		}
	}

	size_t leftBlackHeight = 0;
	size_t rightBlackHeight = 0;
	size_t leftNodeCount = 0;
	size_t rightNodeCount = 0;

	if(!verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::left(*node), leftBlackHeight, ancestryStack, leftNodeCount) ||
	   !verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::right(*node), rightBlackHeight, ancestryStack, rightNodeCount)){
		return false;
	}

	if(leftBlackHeight != rightBlackHeight){
		return false;
	}

	blackHeight = (getColor(*node) == Color::Black) ? (leftBlackHeight + virtualBlackHeight) : leftBlackHeight;
	nodeCount = leftNodeCount + rightNodeCount + virtualNodeCount;
	return true;
}

template <typename StackType>
void verifyAlmostRedBlackTree(const StackType& ancestryStack){
	size_t blackHeight = 0;
	size_t nodeCount = 0;
	for(size_t i = 0; i < ancestryStack.getSize() - 1; i++){
		assert(ancestryStack[i] != ancestryStack[i + 1], "verifyAlmostRedBlackTree - Ancestry stack has duplicate entry");
		assert(ancestryStack[i] != nullptr, "verifyAlmostRedBlackTree - Ancestry stack has null entry that isn't top");
		assert(isChild(**ancestryStack[i], *ancestryStack[i + 1]), "verifyAlmostRedBlackTree - Ancestry stack is invalid");
	}
	assert(verifyAlmostRedBlackTreeImpl(this -> root, blackHeight, ancestryStack, nodeCount), "Almost RBT verification failed");
}

size_t getTreeSize(NodeType* node){
	if(node == nullptr){
		return 0;
	}
	return 1 + getTreeSize(RedBlackTreeInfoExtractor::left(*node)) + getTreeSize(RedBlackTreeInfoExtractor::right(*node));
}

size_t getTreeSize(){
	return getTreeSize(this -> root);
}

template <typename StackType>
requires IsStack<NodeType**, StackType>
//In this case, *ancestryStack[-1] will always be nullptr, pointing to the node that was just deleted.
//Then *ancestryStack[-2] points to the parent of the just-deleted node, *ancestryStack[-3] to the grandparent, etc.
//If we were to insert a black node there, we would have a valid RBT. Now we fix up the state of the tree.
void eraseFixup(StackType& ancestryStack){
	while(ancestryStack.getSize() > 1){
		NodeType* current = *ancestryStack[-1];
		NodeType* parent = *ancestryStack[-2];
		Direction direction = getChildDirection(*parent, current);
		NodeType** sibling = &getChild(*parent, opposite(direction));
#ifdef PARANOID_RBT_VERIFICATION
		verifyAlmostRedBlackTree(ancestryStack);
#endif
		if(getColor(**sibling) == Color::Red){
			setColor(**sibling, Color::Black);
			setColor(*parent, Color::Red);
			rotateAboutParent(ancestryStack, direction);
			continue;
		}

		NodeType* nearNephew = getChild(**sibling, direction);
		NodeType* farNephew = getChild(**sibling, opposite(direction));

		bool nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
		bool farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);

		if(!nearNephewIsRed && !farNephewIsRed){
			setColor(**sibling, Color::Red);
			if(getColor(*parent) == Color::Red){
				setColor(*parent, Color::Black);
				break;
			}
			ancestryStack.pop();
			continue;
		}

		if(!farNephewIsRed && nearNephewIsRed){
			setColor(*nearNephew, Color::Black);
			setColor(**sibling, Color::Red);
			rotateSubtree(*sibling, opposite(direction));
			sibling = &getChild(*parent, opposite(direction));
			nearNephew = getChild(**sibling, direction);
			farNephew = getChild(**sibling, opposite(direction));

			nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
			farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);
		}

		if(farNephewIsRed){
			setColor(**sibling, getColor(*parent));
			setColor(*parent, Color::Black);
			setColor(*farNephew, Color::Black);
			rotateAboutParent(ancestryStack, direction);
			break;
		}
	}
	if(this -> root != nullptr){
		setColor(*this -> root, Color::Black);
	}
}

//If the node has only one child, that child is red. We can replace toReplace with its child and recolor it
//to black
template <Direction direction>
void eraseCaseSingleChild(NodeType*& node){
    if constexpr(direction == Direction::Left){
        node = RedBlackTreeInfoExtractor::left(*node);
        setColor(*node, Color::Black);
    }
    else{
        node = RedBlackTreeInfoExtractor::right(*node);
        setColor(*node, Color::Black);
    }
}

//If the node we're erasing has 2 children, we have to replace it with its successor and possibly run a tree fixup
//if the successor is black.
template <typename StackType>
requires IsStack<NodeType**, StackType>
Color eraseCaseTwoChildren(NodeType*& node, StackType& ancestryStack){
	size_t ancestryStackFixupIndex = ancestryStack.getSize();
    NodeType** successorRef = &RedBlackTreeInfoExtractor::right(*node);
	ancestryStack.push(successorRef);
	while(hasLeftChild(*(*successorRef))){
		successorRef = &RedBlackTreeInfoExtractor::left(*(*successorRef));
		ancestryStack.push(successorRef);
	}
	Color originalColor = getColor(*node);
	Color successorColor = getColor(*(*successorRef));
	NodeType* successor = *successorRef;

	RedBlackTreeInfoExtractor::left(*successor) = RedBlackTreeInfoExtractor::left(*node);
	//If the successor is not immediately to the right, also update its right child
	if(successor != RedBlackTreeInfoExtractor::right(*node)){
		*successorRef = RedBlackTreeInfoExtractor::right(*successor);
		//If the successor is not immediately to the right, but it does have a right child, then that child is red.
		//Recolor it to black and we're done
		if(*successorRef != nullptr){
			setColor(*(*successorRef), Color::Black);
			successorColor = Color::Red;
		}
		RedBlackTreeInfoExtractor::right(*successor) = RedBlackTreeInfoExtractor::right(*node);
	}
	//Otherwise the successor has no left child. Thus it possibly has a red leaf. If so, we can just recolor it to black
	//and we're done
	else{
		if(hasRightChild(*successor)){
			setColor(*RedBlackTreeInfoExtractor::right(*successor), Color::Black);
			successorColor = Color::Red;
		}
	}
	node = successor;
	setColor(*successor, originalColor);
	ancestryStack[ancestryStackFixupIndex] = &RedBlackTreeInfoExtractor::right(*node);
	return successorColor;
}

template <typename StackType>
requires IsStack<NodeType**, StackType>
//Requires that ancestryStack.top() == &toRemove
//The caller must populate the ancestry stack up to the point of toRemove
void eraseImpl(NodeType*& toRemove, StackType& ancestryStack){
    bool performFixup = false;
#ifdef PARANOID_RBT_VERIFICATION
	size_t preRemovalSize = getTreeSize();
#endif
    //Address simple cases first, like the degenerate case when toRemove == null
    if(toRemove == nullptr){}
    //If the node is a leaf, we just remove it. We need to perform a fixup if the node is not the root but is black
    else if(!hasChild(*toRemove)){
        if((toRemove != this -> root) && (getColor(*toRemove) == Color::Black)){
            performFixup = true;
        }
        toRemove = nullptr;
    }
    //If the node is not a leaf, there are 3 simple cases, and one case that may necessitate running a fixup
    else if(hasChild(*toRemove)){
        if(!hasLeftChild(*toRemove)){
            eraseCaseSingleChild<Direction::Right>(toRemove);
        }
        else if(!hasRightChild(*toRemove)){
            eraseCaseSingleChild<Direction::Left>(toRemove);
        }
        else{
            performFixup = (eraseCaseTwoChildren(toRemove, ancestryStack) == Color::Black);
        }
    }
    if(performFixup){
#ifdef PARANOID_RBT_VERIFICATION
		verifyAlmostRedBlackTree(ancestryStack);
		assert(*ancestryStack[-1] == nullptr, "verifyAlmostRedBlackTree - Ancestry stack is invalid (top does not point to null)");
#endif
        eraseFixup(ancestryStack);
    }
#ifdef PARANOID_RBT_VERIFICATION
	assert(getTreeSize() == preRemovalSize - 1, "Node count mismatch");
	verifyRedBlackTree();
#endif
}

template <typename StackType>
requires IsStack<NodeType**, StackType>
void insertFixup(StackType& ancestryStack){
    while(ancestryStack.getSize() >= 3){
		NodeType* node = *ancestryStack[-1];       // current node
		NodeType* parent = *ancestryStack[-2];     // parent
		NodeType* grandparent = *ancestryStack[-3]; // grandparent

		//If the parent is black, tree is already balanced
        if(!RedBlackTreeInfoExtractor::isRed(*parent)){
			break;
		}

		// Parent is red, so we need to fix
		Direction parentDirection = getChildDirection(*grandparent, parent);
		NodeType* uncle = getChild(*grandparent, opposite(parentDirection));

		if(uncle != nullptr && RedBlackTreeInfoExtractor::isRed(*uncle)){
			// Case 1: Uncle is red - recolor
			setColor(*parent, Color::Black);
			setColor(*uncle, Color::Black);
			setColor(*grandparent, Color::Red);

			// Move up the tree - grandparent becomes the new focus
			ancestryStack.pop();
			ancestryStack.pop();
		} else {
			// Case 2: Uncle is black - rotation needed
			Direction nodeDirection = getChildDirection(*parent, node);

			if(nodeDirection != parentDirection){
				// Case 2a: Triangle - first rotation
				rotateSubtree(*ancestryStack[-2], parentDirection);
			}

			// Case 2b: Line - rotate grandparent
			rotateSubtree(*ancestryStack[-3], opposite(parentDirection));

			// Recolor after rotations
			NodeType* newParent = *ancestryStack[-3]; // This is now the top after rotation
			setColor(*newParent, Color::Black);
			if(auto child = RedBlackTreeInfoExtractor::left(*newParent)){
				setColor(*child, Color::Red);
			}
			if(auto child = RedBlackTreeInfoExtractor::right(*newParent)){
				setColor(*child, Color::Red);
			}
			break;
		}
    }

    // Root must always be black
    if(this->root != nullptr){
        RedBlackTreeInfoExtractor::setRed(*(this->root), false);
    }
}

template <typename StackType>
requires IsStack<NodeType**, StackType>
bool insertImpl(NodeType* node){
    if(node == nullptr){
        return false;
    }
    if(this -> root == nullptr){
        this -> root = node;
		setColor(*node, Color::Black);
        return true;
    }
    StackType ancestryStack;
    NodeType** current = &this -> root;
    do{
        ancestryStack.push(current);
        //if node is already present in the tree, bail
		auto& nodeData = RedBlackTreeInfoExtractor::data(*node);
		auto& currentData = RedBlackTreeInfoExtractor::data(*(*current));
        if(nodeData == currentData){
            return false;
        }
        //if node < current, go left
        if(comparator(nodeData, currentData)){
            current = &RedBlackTreeInfoExtractor::left(*(*current));
        }
        //otherwise go right
        else{
            current = &RedBlackTreeInfoExtractor::right(*(*current));
        }
    }while(*current != nullptr);
    *current = node;
	RedBlackTreeInfoExtractor::setRed(*node, true);
    ancestryStack.push(current);
    insertFixup(ancestryStack);
	return true;
}

public:

using BSTParent::visitDepthFirstInOrder;
using BSTParent::visitDepthFirstReverseOrder;
using BSTParent::visitDepthFirstPostOrder;
using BSTParent::find;
using BSTParent::floor;
using BSTParent::ceil;
using BSTParent::successor;
using BSTParent::predecessor;

template <typename StackType>
requires IsStack<NodeType**, StackType>
bool insert(NodeType* node){
#ifdef PARANOID_RBT_VERIFICATION
	size_t preInsertionSize = getTreeSize();
	verifyRedBlackTree();
	bool result = insertImpl<StackType>(node);
	assert(getTreeSize() == preInsertionSize + (result ? 1 : 0), "Node count mismatch");
	verifyRedBlackTree();
	return result;
#else
	return insertImpl<StackType>(node);
#endif
}

template <typename StackType>
NodeType* erase(NodeType* node){
	return erase<StackType>(RedBlackTreeInfoExtractor::data(*node));
}

template <typename StackType>
NodeType* erase(const NodeData& value){
	StackType ancestryStack;
	NodeType** current = &this -> root;
	while(*current != nullptr){
		ancestryStack.push(current);
		auto data = RedBlackTreeInfoExtractor::data(*(*current));
		if(value == data){
			NodeType* toRemove = *current;
			//ancestryStack[0] = root, ancestryStack[-1] is node to be removed
			eraseImpl(*current, ancestryStack);
			return toRemove;
		}
		if(comparator(value, data)){
			current = &RedBlackTreeInfoExtractor::left(*(*current));
		}
		else{
			current = &RedBlackTreeInfoExtractor::right(*(*current));
		}
	}
	return nullptr;
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
        const Node* node = Parent::floor(value);
        if (node != nullptr) {
            result = node->data;
            return true;
        }
        return false;
    }
    
    // Ceil operation - smallest element >= value  
    bool ceil(const T& value, T& result) const {
        const Node* node = Parent::ceil(value);
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
            const Node* succ = Parent::successor(node);
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
            const Node* pred = Parent::predecessor(node);
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

// Red-black tree node for value-owning trees
template<typename T>
struct RedBlackTreeNode {
    T data;
    RedBlackTreeNode* left;
    RedBlackTreeNode* right;
    bool isRed;
    
    RedBlackTreeNode(const T& value) : data(value), left(nullptr), right(nullptr), isRed(true) {}
    RedBlackTreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr), isRed(true) {}
};

// Extractor for RedBlackTreeNode
template<typename T>
struct RedBlackTreeNodeExtractor {
    static RedBlackTreeNode<T>*& left(RedBlackTreeNode<T>& node) { return node.left; }
    static RedBlackTreeNode<T>*& right(RedBlackTreeNode<T>& node) { return node.right; }
    static RedBlackTreeNode<T>* const& left(const RedBlackTreeNode<T>& node) { return node.left; }
    static RedBlackTreeNode<T>* const& right(const RedBlackTreeNode<T>& node) { return node.right; }
    static T& data(RedBlackTreeNode<T>& node) { return node.data; }
    static const T& data(const RedBlackTreeNode<T>& node) { return node.data; }
    static bool isRed(const RedBlackTreeNode<T>& node) { return node.isRed; }
    static void setRed(RedBlackTreeNode<T>& node, bool red) { node.isRed = red; }
};

// Value-owning Red-Black Tree
template<typename T, typename Comparator = DefaultComparator<T>, typename StackType = StaticStack<RedBlackTreeNode<T>**, 64>>
class RedBlackTree : private IntrusiveRedBlackTree<RedBlackTreeNode<T>, RedBlackTreeNodeExtractor<T>, Comparator> {
    using Node = RedBlackTreeNode<T>;
    using Parent = IntrusiveRedBlackTree<Node, RedBlackTreeNodeExtractor<T>, Comparator>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    RedBlackTree() = default;
    explicit RedBlackTree(Comparator comp) : Parent() {
        this->comparator = comp;
    }
    
    ~RedBlackTree() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment
    RedBlackTree(const RedBlackTree&) = delete;
    RedBlackTree& operator=(const RedBlackTree&) = delete;
    
    // Move constructor and assignment
    RedBlackTree(RedBlackTree&& other) noexcept : Parent() {
        this->root = other.root;
        this->comparator = move(other.comparator);
        other.root = nullptr;
    }
    
    RedBlackTree& operator=(RedBlackTree&& other) noexcept {
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
        if(!Parent::template insert<StackType>(node)){
			delete node;
		}
    }
    
    void insert(T&& value) {
        Node* node = new Node(move(value));
		if(!Parent::template insert<StackType>(node)){
			delete node;
		}
    }
    
    // Find operation
    bool contains(const T& value) const {
        return Parent::find(value) != nullptr;
    }
    
    // Erase operation
    bool erase(const T& value) {
		auto erasedNode = Parent::template erase<StackType>(value);
		if(erasedNode != nullptr){
			delete erasedNode;
			return true;
		}
		return false;
    }
    
    // Floor operation - largest element <= value
    bool floor(const T& value, T& result) const {
        const Node* node = Parent::floor(value);
        if (node != nullptr) {
            result = node->data;
            return true;
        }
        return false;
    }
    
    // Ceil operation - smallest element >= value  
    bool ceil(const T& value, T& result) const {
        const Node* node = Parent::ceil(value);
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
            const Node* succ = Parent::successor(node);
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
            const Node* pred = Parent::predecessor(node);
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

	const RedBlackTreeNode<T>* getRoot() const { return this->root; }
};

#endif //TREES_H
