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

template <typename NodeType, typename BinaryTreeInfoExtractor>
concept AugmentedBinaryTreeNodeType = requires(NodeType& node, const NodeType& cnode, const BinaryTreeInfoExtractor extractor){
	requires IsReference<decltype(BinaryTreeInfoExtractor::augmentedData(node))>;
	{BinaryTreeInfoExtractor::recomputeAugmentedData(cnode, &cnode, &cnode)} -> IsSame<remove_reference_t<decltype(BinaryTreeInfoExtractor::augmentedData(node))>>;
};

template <typename NodeType, typename BinaryTreeInfoExtractor>
concept ParentPointerBinaryTreeNodeType = requires(NodeType& node, const BinaryTreeInfoExtractor extractor){
	{BinaryTreeInfoExtractor::parent(node)} -> IsSame<NodeType*&>;
};

struct NoAugmentation{};

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
		if constexpr (ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			BinaryTreeInfoExtractor::parent(*newRoot) = BinaryTreeInfoExtractor::parent(*pivot);
			BinaryTreeInfoExtractor::parent(*pivot) = newRoot;
			auto* child = BinaryTreeInfoExtractor::right(*pivot);
			if(child != nullptr){
				BinaryTreeInfoExtractor::parent(*child) = pivot;
			}
		}
		if constexpr (AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*pivot, BinaryTreeInfoExtractor::left(*pivot), BinaryTreeInfoExtractor::right(*pivot));
				BinaryTreeInfoExtractor::augmentedData(*pivot) = recomputedAugmentedData ;
			}
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*newRoot, BinaryTreeInfoExtractor::left(*newRoot), BinaryTreeInfoExtractor::right(*newRoot));
				BinaryTreeInfoExtractor::augmentedData(*newRoot) = recomputedAugmentedData;
			}
		}
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
		if constexpr (ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			BinaryTreeInfoExtractor::parent(*newRoot) = BinaryTreeInfoExtractor::parent(*pivot);
			BinaryTreeInfoExtractor::parent(*pivot) = newRoot;
			auto* child = BinaryTreeInfoExtractor::left(*pivot);
			if(child != nullptr){
				BinaryTreeInfoExtractor::parent(*child) = pivot;
			}
		}
		if constexpr (AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*pivot, BinaryTreeInfoExtractor::left(*pivot), BinaryTreeInfoExtractor::right(*pivot));
				BinaryTreeInfoExtractor::augmentedData(*pivot) = recomputedAugmentedData;
			}
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*newRoot, BinaryTreeInfoExtractor::left(*newRoot), BinaryTreeInfoExtractor::right(*newRoot));
				BinaryTreeInfoExtractor::augmentedData(*newRoot) = recomputedAugmentedData;
			}
		}
        return true;
    }

	bool verifyAugmentationData(NodeType* node) const
	requires AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor> {
		if(node == nullptr){
			return true;
		}
		bool leftValid = verifyAugmentationData(BinaryTreeInfoExtractor::left(*node));
		bool rightValid = verifyAugmentationData(BinaryTreeInfoExtractor::right(*node));
		if(!leftValid || !rightValid){
			return false;
		}
		auto augmentedData = BinaryTreeInfoExtractor::augmentedData(*node);
		auto computedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*node, BinaryTreeInfoExtractor::left(*node), BinaryTreeInfoExtractor::right(*node));
		return augmentedData == computedAugmentedData;
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

static constexpr bool HasParentPointer = ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>;
static constexpr bool AugmentedNode = AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>;

protected:
	//Annoying nonsense in case multiple nodes have the same data
	NodeType** findParentPointerTreeTraverse(const NodeType* node, NodeType** searchRoot) const requires (!HasParentPointer){
		if(searchRoot == nullptr){return nullptr;}
		if(*searchRoot == node){return searchRoot;}
		if(BinaryTreeInfoExtractor::data(*node) != BinaryTreeInfoExtractor::data(**searchRoot)){return nullptr;}
		NodeType** possible = findParentPointerTreeTraverse(node, BinaryTreeInfoExtractor::left(**searchRoot));
		if(possible != nullptr){return possible;}
		return findParentPointerTreeTraverse(node, BinaryTreeInfoExtractor::right(**searchRoot));
	}

	NodeType** findParentPointer(const NodeType* node) const requires (!HasParentPointer){
		NodeType** current = &this -> root;
		if(node == nullptr){return nullptr;}
		NodeData& value = BinaryTreeInfoExtractor::data(*node);
		while(*current != nullptr){
			NodeData& currentValue = BinaryTreeInfoExtractor::data(**current);
			if(value == currentValue){
				return findParentPointerTreeTraverse(node, current);
			}
			else if(comparator(value, currentValue)){
				current = &BinaryTreeInfoExtractor::right(**current);
			}
			else{
				current = &BinaryTreeInfoExtractor::left(**current);
			}
		}
		return nullptr;
	}

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeData& targetValue, StackType& ancestryStack) requires (!HasParentPointer){
		NodeType** current = &this -> root;
		ancestryStack.push(current);
		while(*current != nullptr){
			NodeData& value = BinaryTreeInfoExtractor::data(**current);
			if(targetValue == value){return true;}
			//if value < targetValue, go right
			if(comparator(value, targetValue)){
				current = &BinaryTreeInfoExtractor::right(**current);
			}
			else{
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(current);
		}
		return false;
	}

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	//Populates the ancestry stack starting at node. *ancestryStack[-1] is either if the stack is empty,
	//points to the would-be parent after a BST insert if the node does not exist
	//or points to the node containing targetValue
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeData& targetValue, StackType& ancestryStack) requires (!HasParentPointer){
		NodeType* current = this -> root;
		while(current != nullptr){
			ancestryStack.push(current);
			NodeData& value = BinaryTreeInfoExtractor::data(*current);
			if(targetValue == value){return true;}
			if(comparator(value, targetValue)){
				current = BinaryTreeInfoExtractor::right(*current);
			}
			else{
				current = BinaryTreeInfoExtractor::left(*current);
			}
		}
		return false;
	}


	//More annoying nonsense in case multiple nodes have the same data
	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	bool tryFindTargetNode(NodeType* targetNode, NodeType* searchNode, StackType& ancestryStack) requires (!HasParentPointer){
		if(searchNode == nullptr){return false;}
		if(searchNode == targetNode){return true;}
		if(BinaryTreeInfoExtractor::data(*targetNode) != BinaryTreeInfoExtractor::data(*searchNode)){return false;}
		ancestryStack.push(&BinaryTreeInfoExtractor::left(*searchNode));
		bool found = tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::left(*searchNode), ancestryStack);
		if(found){return true;}
		ancestryStack.pop();
		ancestryStack.push(&BinaryTreeInfoExtractor::right(*searchNode));
		return tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::right(*searchNode), ancestryStack);
	}

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	bool tryFindTargetNode(NodeType* targetNode, NodeType* searchNode, StackType& ancestryStack) requires (!HasParentPointer){
		if(searchNode == nullptr){return false;}
		if(searchNode == targetNode){return true;}
		if(BinaryTreeInfoExtractor::data(*targetNode) != BinaryTreeInfoExtractor::data(*searchNode)){return false;}
		ancestryStack.push(BinaryTreeInfoExtractor::left(*searchNode));
		bool found = tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::left(*searchNode), ancestryStack);
		if(found){return true;}
		ancestryStack.pop();
		ancestryStack.push(BinaryTreeInfoExtractor::right(*searchNode));
		return tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::right(*searchNode), ancestryStack);
	}

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeType* targetNode, StackType& ancestryStack) requires (!HasParentPointer){
		NodeType** current = &this -> root;
		ancestryStack.push(current);
		const NodeData& targetValue = BinaryTreeInfoExtractor::data(*targetNode);
		while(*current != nullptr){
			NodeData& value = BinaryTreeInfoExtractor::data(**current);
			if(targetValue == value){
				return tryFindTargetNode(targetNode, *current, ancestryStack);
			}
			//if value < targetValue, go right
			if(comparator(value, targetValue)){
				current = &BinaryTreeInfoExtractor::right(**current);
			}
			else{
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(current);
		}
		return false;
	}

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeType* targetNode, StackType& ancestryStack) requires (!HasParentPointer){
		NodeType** current = &this -> root;
		ancestryStack.push(*current);
		const NodeData& targetValue = BinaryTreeInfoExtractor::data(*targetNode);
		while(*current != nullptr){
			NodeData& value = BinaryTreeInfoExtractor::data(**current);
			if(targetValue == value){
				return tryFindTargetNode(targetNode, *current, ancestryStack);
			}
			//if value < targetValue, go right
			if(comparator(value, targetValue)){
				current = &BinaryTreeInfoExtractor::right(**current);
			}
			else{
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(*current);
		}
		return false;
	}

	bool updateNodeAugmentationData(NodeType* node) requires AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>{
		if(node == nullptr){return false;}
		auto oldData = BinaryTreeInfoExtractor::augmentedData(*node);
		auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(*node, BinaryTreeInfoExtractor::left(*node), BinaryTreeInfoExtractor::right(*node));
		BinaryTreeInfoExtractor::augmentedData(*node) = recomputedAugmentedData;
		return oldData == recomputedAugmentedData;
	}

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	void fixupAugmentationData(StackType& ancestryStack) requires AugmentedNode && (!HasParentPointer){
		while(!ancestryStack.empty()){
			updateNodeAugmentationData(ancestryStack.pop());
		}
	}

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	void fixupAugmentationData(StackType& ancestryStack) requires AugmentedNode && (!HasParentPointer){
		while(!ancestryStack.empty()){
			updateNodeAugmentationData(*ancestryStack.pop());
		}
	}

	void fixupAugmentationData(NodeType* node) requires AugmentedNode && HasParentPointer{
		if(this -> root == nullptr){return;}

		NodeType* current = node;

		do{
			updateNodeAugmentationData(current);
		} while(current = BinaryTreeInfoExtractor::parent(*current));
	}

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	void propagateAugmentationRefresh(StackType& ancestryStack) requires AugmentedNode && (!HasParentPointer){
		while(!ancestryStack.empty()){
			if(updateNodeAugmentationData(ancestryStack.pop())) return;
		}
	}

	void propagateAugmentationRefresh(NodeType& node) requires AugmentedNode && HasParentPointer{
		NodeType* current = node;
		do{
			if(updateNodeAugmentationData(current)) return;
		} while(current = BinaryTreeInfoExtractor::parent(*current));
	}

    Comparator comparator;

    void insert(NodeType* toInsert, NodeType*& root) requires (!AugmentedNode || HasParentPointer) {
		NodeType* parent = nullptr;
        NodeType** current = &root;

        while(*current != nullptr){
			parent = *current;
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

		if constexpr(HasParentPointer){
			BinaryTreeInfoExtractor::parent(*toInsert) = parent;
			if constexpr(AugmentedNode){
				propagateAugmentationRefresh(*toInsert);
			}
		}
    }
	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	void insert(NodeType* toInsert, NodeType*& root) requires (AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
        NodeType** current = &root;

        while(*current != nullptr){
			ancestryStack.push(*current);
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

		ancestryStack.push(toInsert);
		propagateAugmentationRefresh(ancestryStack);
	}

    NodeType** findImpl(const remove_reference_t<NodeData>& value, NodeType** root) const{
        NodeType** current = root;
        while(*current != nullptr){
            if(value == BinaryTreeInfoExtractor::data(**current)){
                return current;
            }
            //if value < current, go left
            if(comparator(value, BinaryTreeInfoExtractor::data(**current))){
                current = &BinaryTreeInfoExtractor::left(**current);
            }
            //otherwise go right
            else{
                current = &BinaryTreeInfoExtractor::right(**current);
            }
        }
        return nullptr;
    }
    
    const NodeType* const* findImpl(const remove_reference_t<NodeData>& value, const NodeType* const* root) const{
        const NodeType* const* current = root;
        while(*current != nullptr){
            if(value == BinaryTreeInfoExtractor::data(**current)){
                return current;
            }
            //if value < current, go left
            if(comparator(value, BinaryTreeInfoExtractor::data(**current))){
                current = &BinaryTreeInfoExtractor::left(**current);
            }
            //otherwise go right
            else{
                current = &BinaryTreeInfoExtractor::right(**current);
            }
        }
        return nullptr;
    }

    NodeType* eraseNodeImpl(NodeType** toRemove) requires (!AugmentedNode || HasParentPointer) {
        NodeType* toReturn = *toRemove;
        //If one of the children of the node we are trying to erase is null, deletion is simple: just replace
        //the pointer to toRemove in its parent with the other child
		NodeType* parent = nullptr;
        if(BinaryTreeInfoExtractor::left(*(*toRemove)) == nullptr){
			if constexpr(HasParentPointer){
				parent = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}
            *toRemove = BinaryTreeInfoExtractor::right(*(*toRemove));
			//It's possible toRemove is a leaf in this case
			if constexpr(HasParentPointer){
				if(*toRemove != nullptr){
					BinaryTreeInfoExtractor::parent(*(*toRemove)) = parent;
				}
			}
			if constexpr(AugmentedNode){
				fixupAugmentationData(parent);
			}
        }
        else if(BinaryTreeInfoExtractor::right(*(*toRemove)) == nullptr){
			if constexpr(HasParentPointer){
            	parent = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}
            *toRemove = BinaryTreeInfoExtractor::left(*(*toRemove));
			//But here, we know toRemove's left child is not null, so no need for a null check
			if constexpr(HasParentPointer){
				BinaryTreeInfoExtractor::parent(*(*toRemove)) = parent;
			}
			if constexpr(AugmentedNode){
				fixupAugmentationData(parent);
			}
        }
        //Otherwise, we need to find the successor of toRemove, and replace it with the successor
		//Notably the successor is not toRemove's immediate parent since BinaryTreeInfoExtractor::right is not null
        else{
            //Find the successor of toRemove
            NodeType** successor = &BinaryTreeInfoExtractor::right(*(*toRemove));
			bool immediateChild = true;
            while(BinaryTreeInfoExtractor::left(*(*successor)) != nullptr){
				immediateChild = false;
                successor = &BinaryTreeInfoExtractor::left(*(*successor));
            }

			if(immediateChild){
				auto leftChild = BinaryTreeInfoExtractor::left(**toRemove);
				BinaryTreeInfoExtractor::left(**successor) = leftChild;

				if constexpr(HasParentPointer){
					if(leftChild != nullptr){
						BinaryTreeInfoExtractor::parent(*leftChild) = *successor;
					}
					BinaryTreeInfoExtractor::parent(**successor) = BinaryTreeInfoExtractor::parent(**toRemove);
				}
				*toRemove = *successor;
				if constexpr(AugmentedNode){
					fixupAugmentationData(*toRemove);
				}
				return toReturn;
			}
			//In this case, we know successor is not the immediate child of toReplace.
            //Retain a pointer to the successor, as we will be replacing it with its right child
            NodeType* successorPtr = *successor;

			if constexpr(HasParentPointer){
				parent = BinaryTreeInfoExtractor::parent(*successorPtr);
			}
            //Remove the successor from the tree, replace it with its right child. The left will always be empty
            *successor = BinaryTreeInfoExtractor::right(*successorPtr);
			if constexpr(HasParentPointer){
				if(*successor != nullptr){
					BinaryTreeInfoExtractor::parent(*(*successor)) = parent;
				}
				BinaryTreeInfoExtractor::parent(*successorPtr) = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}

            //Now copy the child pointers from toRemove to the successor
            BinaryTreeInfoExtractor::left(*successorPtr) = BinaryTreeInfoExtractor::left(*(*toRemove));
            BinaryTreeInfoExtractor::right(*successorPtr) = BinaryTreeInfoExtractor::right(*(*toRemove));

			if constexpr(HasParentPointer){
				NodeType* left = BinaryTreeInfoExtractor::left(*successorPtr);
				NodeType* right = BinaryTreeInfoExtractor::right(*successorPtr);
				if(left != nullptr){
					BinaryTreeInfoExtractor::parent(*left) = successorPtr;
				}
				if(right != nullptr){
					BinaryTreeInfoExtractor::parent(*right) = successorPtr;
				}
			}

            //update the parent pointer toRemove to point to its successor
            *toRemove = successorPtr;

			if constexpr(AugmentedNode){
				fixupAugmentationData(parent);
			}
        }
        return toReturn;
    }

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	NodeType* eraseNodeImpl(StackType& ancestryStack) requires (AugmentedNode && !HasParentPointer) {
		if(ancestryStack.empty()){return nullptr;}
		NodeType** toRemove = ancestryStack[-1];
		NodeType* toReturn = *toRemove;
		//If the left child is empty, just replace toRemove with its right child
		if(BinaryTreeInfoExtractor::left(*(*toRemove)) == nullptr){
			*toRemove = BinaryTreeInfoExtractor::right(*(*toRemove));
			fixupAugmentationData(ancestryStack);
		}
		else if(BinaryTreeInfoExtractor::right(*(*toRemove)) == nullptr){
			*toRemove = BinaryTreeInfoExtractor::left(*(*toRemove));
			fixupAugmentationData(ancestryStack);
		}
		else{
			NodeType** successor = &BinaryTreeInfoExtractor::right(*(*toRemove));
			ancestryStack.push(successor);
			bool immediateChild = true;
            while(BinaryTreeInfoExtractor::left(*(*successor)) != nullptr){
				immediateChild = false;
                successor = &BinaryTreeInfoExtractor::left(*(*successor));
				ancestryStack.push(successor);
            }
			if(immediateChild){
				BinaryTreeInfoExtractor::left(**successor) = BinaryTreeInfoExtractor::left(**toRemove);
				*toRemove = *successor;
				fixupAugmentationData(ancestryStack);
				return toReturn;
			}

			NodeType* successorPtr = *successor;
			*successor = BinaryTreeInfoExtractor::right(*successorPtr);
			BinaryTreeInfoExtractor::left(*successorPtr) = BinaryTreeInfoExtractor::left(*(*toRemove));
			BinaryTreeInfoExtractor::right(*successorPtr) = BinaryTreeInfoExtractor::right(*(*toRemove));
			*toRemove = successorPtr;
			fixupAugmentationData(ancestryStack);
		}
		return toReturn;
	}

    //Returns a pointer to the erased type - in an intrusive environment, it is up to the caller to delete the memory
    //as appropriate
    NodeType* eraseImpl(const remove_reference_t<NodeData>& value, NodeType*& root) requires (!AugmentedNode || HasParentPointer) {
        NodeType** toRemove = this -> findImpl(value, &root);
		if(toRemove == nullptr){return nullptr;}
        return eraseNodeImpl(toRemove);
    }

    //Returns a pointer to the erased type - in an intrusive environment, it is up to the caller to delete the memory
    //as appropriate
    NodeType* eraseImpl(const NodeType* node, NodeType*& root) requires (!AugmentedNode || HasParentPointer){
        NodeType** toRemove = this -> findParentPointerTreeTraverse(node, &root);
		if(toRemove == nullptr){return nullptr;}
        return eraseNodeImpl(toRemove);
    }

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	NodeType* eraseImpl(const remove_reference_t<NodeData>& value, NodeType*& root) requires (AugmentedNode && !HasParentPointer) {
        StackType ancestryStack;
		if(populateAncestryStack(value, ancestryStack)){
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
    }

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	NodeType* eraseImpl(const remove_reference_t<NodeData>& value, NodeType*& root) requires (AugmentedNode && !HasParentPointer) {
        StackType ancestryStack;
		if(populateAncestryStack(value, ancestryStack)){
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
    }

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	NodeType* eraseImpl(const NodeType* node, NodeType*& root) requires (AugmentedNode && !HasParentPointer) {
        StackType ancestryStack;
		if(populateAncestryStack(node, ancestryStack)){
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
    }

	template <typename StackType>
	requires IsStack<NodeType*, StackType>
	NodeType* eraseImpl(const NodeType* node, NodeType*& root) requires (AugmentedNode && !HasParentPointer) {
        StackType ancestryStack;
		if(populateAncestryStack(const_cast<NodeType*>(node), ancestryStack)){
			return eraseNodeImpl(ancestryStack);
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

    void insert(NodeType* node) requires (!AugmentedNode || HasParentPointer){
        this -> insert(node, this -> root);
    }

	template <typename StackType = StaticStack<NodeType*, 64>>
	requires IsStack<NodeType*, StackType>
	void insert(NodeType* node) requires (AugmentedNode && !HasParentPointer) {
        this -> template insert<StackType>(node, this -> root);
    }

	template <typename StackType>
	requires IsStack<NodeType**, StackType>
	void insert(NodeType* node) requires (AugmentedNode && !HasParentPointer) {
        this -> template insert<StackType>(node, this -> root);
    }

    NodeType* find(const remove_reference_t<NodeData>& value){
		NodeType** result = this -> findImpl(value, &this -> root);
		if(result == nullptr){return nullptr;}
        return *result;
    }
    
    const NodeType* find(const remove_reference_t<NodeData>& value) const{
		const NodeType* const* result = this -> findImpl(value, &this -> root);
		if(result == nullptr){return nullptr;}
        return *result;
    }

    NodeType* erase(const remove_reference_t<NodeData>& value) requires (!AugmentedNode || HasParentPointer){
        return this -> eraseImpl(value, this -> root);
    }

    NodeType* erase(NodeType* value) requires (!AugmentedNode || HasParentPointer){
        return this -> eraseImpl(value, this -> root);
    }

	template<typename StackType = StaticStack<NodeType*, 64>>
	requires IsStack<NodeType*, StackType>
	NodeType* erase(const remove_reference_t<NodeData>& value) requires (AugmentedNode && !HasParentPointer){
        return this -> template eraseImpl<StackType>(value, this -> root);
    }

	template<typename StackType>
	requires IsStack<NodeType**, StackType>
	NodeType* erase(const remove_reference_t<NodeData>& value) requires (AugmentedNode && !HasParentPointer){
        return this -> template eraseImpl<StackType>(value, this -> root);
    }

	template<typename StackType = StaticStack<NodeType*, 64>>
	requires IsStack<NodeType*, StackType>
    NodeType* erase(NodeType* node) requires (AugmentedNode && !HasParentPointer){
        return this -> template eraseImpl<StackType>(node, this -> root);
    }

	template<typename StackType>
	requires IsStack<NodeType**, StackType>
    NodeType* erase(NodeType* node) requires (AugmentedNode && !HasParentPointer){
        return this -> template eraseImpl<StackType>(node, this -> root);
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
static constexpr bool HasParentPointer = ParentPointerBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;
static constexpr bool AugmentedNode = AugmentedBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;

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
void rotateAboutParent(StackType& ancestryStack, Direction direction) requires (!HasParentPointer){
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
void eraseFixup(StackType& ancestryStack) requires (!HasParentPointer){
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

void eraseFixup(NodeType& node) requires (hasParentPointer){
	NodeType* current = &node;
	do{
		NodeType* parent = *ancestryStack[-2];
		Direction direction = getChildDirection(*parent, current);
		NodeType** sibling = &getChild(*parent, opposite(direction));
#ifdef PARANOID_RBT_VERIFICATION
		//verifyAlmostRedBlackTree(ancestryStack);
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
	} while(current = RedBlackTreeInfoExtractor::parent(*current));
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
    if(toRemove == nullptr){return;}
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
	NodeType* parent = nullptr;
	if constexpr (AugmentedNode){
		if(ancestryStack.getSize() > 1){
			parent = *ancestryStack[-2];
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
	if constexpr (AugmentedNode){
		if(parent != nullptr){
			StackType augmentationAncestryStack;
			BSTParent::populateAncestryStack(RedBlackTreeInfoExtractor::data(*parent), augmentationAncestryStack);
			augmentationAncestryStack.push(&RedBlackTreeInfoExtractor::left(*parent));
			BSTParent::fixupAugmentationData(augmentationAncestryStack);
		}
		//assert(BinTreeParent::verifyAugmentationData(this -> root), "Augmentation data verification failed");
	}
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
#endif
	bool result = insertImpl<StackType>(node);
#ifdef PARANOID_RBT_VERIFICATION
	assert(getTreeSize() == preInsertionSize + (result ? 1 : 0), "Node count mismatch");
	verifyRedBlackTree();
#endif
	if constexpr (AugmentedNode){
		StackType ancestryStack;
		BSTParent::populateAncestryStack(RedBlackTreeInfoExtractor::data(*node), ancestryStack);
		BSTParent::fixupAugmentationData(ancestryStack);
		//assert(BinTreeParent::verifyAugmentationData(this -> root), "Augmentation data verification failed");
	}
	return result;
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

using UpdateLambda = FunctionRef<void(NodeType&)>;
bool update(const NodeData& value, UpdateLambda updateLambda){
	NodeType* node = erase(value);
	if(node != nullptr){
		updateLambda(*node);
		insert(node);
		return true;
	}
	return false;
}

void markAugmentedDataDirty(const NodeData& value) requires AugmentedBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>{
//TODO
}
};

// Value-owning tree node for standard (non-intrusive) trees
template<typename T, bool HasParent>
struct TreeNode;

template<typename T>
struct TreeNode<T, false> {
    T data;
    TreeNode* left;
    TreeNode* right;

    TreeNode(const T& value) : data(value), left(nullptr), right(nullptr) {}
    TreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr) {}
};

template<typename T>
struct TreeNode<T, true> {
    T data;
    TreeNode* left;
    TreeNode* right;
	TreeNode* parent;

    TreeNode(const T& value) : data(value), left(nullptr), right(nullptr), parent(nullptr) {}
    TreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr), parent(nullptr) {}
};

// Extractor for TreeNode
template<typename T, bool HasParent>
struct TreeNodeExtractor;

template<typename T>
struct TreeNodeExtractor<T, false> {
    static TreeNode<T, false>*& left(TreeNode<T, false>& node) { return node.left; }
    static TreeNode<T, false>*& right(TreeNode<T, false>& node) { return node.right; }
    static TreeNode<T, false>* const& left(const TreeNode<T, false>& node) { return node.left; }
    static TreeNode<T, false>* const& right(const TreeNode<T, false>& node) { return node.right; }
    static T& data(TreeNode<T, false>& node) { return node.data; }
    static const T& data(const TreeNode<T, false>& node) { return node.data; }
};

template<typename T>
struct TreeNodeExtractor<T, true> {
    static TreeNode<T, true>*& left(TreeNode<T, true>& node) { return node.left; }
    static TreeNode<T, true>*& right(TreeNode<T, true>& node) { return node.right; }
    static TreeNode<T, true>*& parent(TreeNode<T, true>& node) { return node.parent; }
    static TreeNode<T, true>* const& left(const TreeNode<T, true>& node) { return node.left; }
    static TreeNode<T, true>* const& right(const TreeNode<T, true>& node) { return node.right; }
    static TreeNode<T, true>* const& parent(const TreeNode<T, true>& node) { return node.parent; }
    static T& data(TreeNode<T, true>& node) { return node.data; }
    static const T& data(const TreeNode<T, true>& node) { return node.data; }
};

// Value-owning Binary Tree
template<typename T, bool HasParent>
class BinaryTreeBase : private IntrusiveBinaryTree<TreeNode<T, HasParent>, TreeNodeExtractor<T, HasParent>> {
    using Node = TreeNode<T, HasParent>;
    using Parent = IntrusiveBinaryTree<Node, TreeNodeExtractor<T, HasParent>>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    BinaryTreeBase() = default;
    
    // Constructor with root value
    explicit BinaryTreeBase(const T& rootValue) {
        this->root = new Node(rootValue);
    }
    
    explicit BinaryTreeBase(T&& rootValue) {
        this->root = new Node(move(rootValue));
    }
    
    ~BinaryTreeBase() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment - trees are move-only for simplicity
    BinaryTreeBase(const BinaryTreeBase&) = delete;
    BinaryTreeBase& operator=(const BinaryTreeBase&) = delete;
    
    // Move constructor and assignment
    BinaryTreeBase(BinaryTreeBase&& other) noexcept : Parent(other.root) {
        other.root = nullptr;
    }
    
    BinaryTreeBase& operator=(BinaryTreeBase&& other) noexcept {
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

template <typename T>
using BinaryTreeWithoutParents = BinaryTreeBase<T, false>;

template <typename T>
using BinaryTree = BinaryTreeBase<T, true>;

// Value-owning Binary Search Tree
template<typename T, bool HasParent, typename Comparator>
class BinarySearchTreeBase : private IntrusiveBinarySearchTree<TreeNode<T, HasParent>, TreeNodeExtractor<T, HasParent>, Comparator> {
    using Node = TreeNode<T, HasParent>;
    using Parent = IntrusiveBinarySearchTree<Node, TreeNodeExtractor<T, HasParent>, Comparator>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    BinarySearchTreeBase() = default;
    explicit BinarySearchTreeBase(Comparator comp) : Parent() {
        this->comparator = comp;
    }
    
    ~BinarySearchTreeBase() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment
    BinarySearchTreeBase(const BinarySearchTreeBase&) = delete;
    BinarySearchTreeBase& operator=(const BinarySearchTreeBase&) = delete;
    
    // Move constructor and assignment
    BinarySearchTreeBase(BinarySearchTreeBase&& other) noexcept : Parent() {
        this->root = other.root;
        this->comparator = move(other.comparator);
        other.root = nullptr;
    }
    
    BinarySearchTreeBase& operator=(BinarySearchTreeBase&& other) noexcept {
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

template<typename T, typename Comparator = DefaultComparator<T>>
using BinarySearchTreeWithoutParents = BinarySearchTreeBase<T, false, Comparator>;

template<typename T, typename Comparator = DefaultComparator<T>>
using BinarySearchTree = BinarySearchTreeBase<T, true, Comparator>;

// Red-black tree node for value-owning trees
template<typename T>
struct PlainRedBlackTreeNode {
    T data;
    PlainRedBlackTreeNode* left;
    PlainRedBlackTreeNode* right;
    bool isRed;

    PlainRedBlackTreeNode(const T& value) : data(value), left(nullptr), right(nullptr), isRed(true) {}
    PlainRedBlackTreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr), isRed(true) {}
};

template<typename T, typename S>
struct AugmentedRedBlackTreeNode {
    T data;
	S augmentationData;
    AugmentedRedBlackTreeNode* left;
    AugmentedRedBlackTreeNode* right;
    bool isRed;

    AugmentedRedBlackTreeNode(const T& value) : data(value), left(nullptr), right(nullptr), isRed(true), augmentationData(S()) {}
    AugmentedRedBlackTreeNode(T&& value) : data(move(value)), left(nullptr), right(nullptr), isRed(true), augmentationData(S()) {}
};

// Extractor for RedBlackTreeNode
template<typename T>
struct PlainRedBlackTreeNodeExtractor {
    static PlainRedBlackTreeNode<T>*& left(PlainRedBlackTreeNode<T>& node) { return node.left; }
    static PlainRedBlackTreeNode<T>*& right(PlainRedBlackTreeNode<T>& node) { return node.right; }
    static PlainRedBlackTreeNode<T>* const& left(const PlainRedBlackTreeNode<T>& node) { return node.left; }
    static PlainRedBlackTreeNode<T>* const& right(const PlainRedBlackTreeNode<T>& node) { return node.right; }
    static T& data(PlainRedBlackTreeNode<T>& node) { return node.data; }
    static const T& data(const PlainRedBlackTreeNode<T>& node) { return node.data; }
    static bool isRed(const PlainRedBlackTreeNode<T>& node) { return node.isRed; }
    static void setRed(PlainRedBlackTreeNode<T>& node, bool red) { node.isRed = red; }
};

// Extractor for RedBlackTreeNode
template<typename T, typename S, typename AugmentationAccumulator>
struct AugmentedRedBlackTreeNodeExtractor {
    static AugmentedRedBlackTreeNode<T, S>*& left(AugmentedRedBlackTreeNode<T, S>& node) { return node.left; }
    static AugmentedRedBlackTreeNode<T, S>*& right(AugmentedRedBlackTreeNode<T, S>& node) { return node.right; }
    static AugmentedRedBlackTreeNode<T, S>* const& left(const AugmentedRedBlackTreeNode<T, S>& node) { return node.left; }
    static AugmentedRedBlackTreeNode<T, S>* const& right(const AugmentedRedBlackTreeNode<T, S>& node) { return node.right; }
    static T& data(AugmentedRedBlackTreeNode<T, S>& node) { return node.data; }
    static const T& data(const AugmentedRedBlackTreeNode<T, S>& node) { return node.data; }
    static bool isRed(const AugmentedRedBlackTreeNode<T, S>& node) { return node.isRed; }
    static void setRed(AugmentedRedBlackTreeNode<T, S>& node, bool red) { node.isRed = red; }
	static S& augmentedData(AugmentedRedBlackTreeNode<T, S>& node) { return node.augmentationData; }
	static const S& augmentedData(const AugmentedRedBlackTreeNode<T, S>& node) { return node.augmentationData; }
	static S recomputeAugmentedData(const AugmentedRedBlackTreeNode<T, S>& node,
		   const AugmentedRedBlackTreeNode<T, S>* left, const AugmentedRedBlackTreeNode<T, S>* right)
	{
		AugmentationAccumulator accumulator;
		const T& nodeData = data(node);
		const S* leftData = (left != nullptr) ? &augmentedData(*left) : nullptr;
		const S* rightData = (right != nullptr) ? &augmentedData(*right) : nullptr;
		return accumulator(nodeData, leftData, rightData);
	}
};

template <typename T, typename AI>
concept ValidAugmentationInfo = requires(const T t, const typename AI::Data* left, const typename AI::Data* right){
	{typename AI::Accumulator{}(t, left, right)} -> IsSame<typename AI::Data>;
};

template <typename T, typename AugmentationInfo>
struct RedBlackTreeNodeHelper;

template <typename T>
struct RedBlackTreeNodeHelper<T, NoAugmentation> {
	using type = PlainRedBlackTreeNode<T>;
};

template<typename T, typename AugmentationInfo>
requires ValidAugmentationInfo<T, AugmentationInfo>
struct RedBlackTreeNodeHelper<T, AugmentationInfo> {
	using type = AugmentedRedBlackTreeNode<T, typename AugmentationInfo::Data>;
};

template<typename T, typename AugmentationInfo>
using RedBlackTreeNode = RedBlackTreeNodeHelper<T, AugmentationInfo>::type;

template <typename T, typename AugmentationInfo>
struct RedBlackTreeInfoExtractorHelper;

template <typename T>
struct RedBlackTreeInfoExtractorHelper<T, NoAugmentation> {
	using type = PlainRedBlackTreeNodeExtractor<T>;
};

template<typename T, typename AugmentationInfo>
requires ValidAugmentationInfo<T, AugmentationInfo>
struct RedBlackTreeInfoExtractorHelper<T, AugmentationInfo> {
	using type = AugmentedRedBlackTreeNodeExtractor<T, typename AugmentationInfo::Data, typename AugmentationInfo::Accumulator>;
};

template<typename T, typename AugmentationInfo>
using RedBlackTreeInfoExtractor = RedBlackTreeInfoExtractorHelper<T, AugmentationInfo>::type;

// Value-owning Red-Black Tree
template<typename T, typename AugmentationInfo = NoAugmentation, typename Comparator = DefaultComparator<T>, typename StackType = StaticStack<RedBlackTreeNode<T, AugmentationInfo>**, 64>>
class GeneralRedBlackTree : private IntrusiveRedBlackTree<RedBlackTreeNode<T, AugmentationInfo>, RedBlackTreeInfoExtractor<T, AugmentationInfo>, Comparator> {
    using Node = RedBlackTreeNode<T, AugmentationInfo>;
    using Parent = IntrusiveRedBlackTree<Node, RedBlackTreeInfoExtractor<T, AugmentationInfo>, Comparator>;
    
    void deleteSubtree(Node* node) {
        if (node != nullptr) {
            deleteSubtree(node->left);
            deleteSubtree(node->right);
            delete node;
        }
    }
    
public:
    GeneralRedBlackTree() = default;
    explicit GeneralRedBlackTree(Comparator comp) : Parent() {
        this->comparator = comp;
    }
    
    ~GeneralRedBlackTree() {
        deleteSubtree(this->root);
    }
    
    // Delete copy constructor and assignment
    GeneralRedBlackTree(const GeneralRedBlackTree&) = delete;
    GeneralRedBlackTree& operator=(const GeneralRedBlackTree&) = delete;
    
    // Move constructor and assignment
    GeneralRedBlackTree(GeneralRedBlackTree&& other) noexcept : Parent() {
        this->root = other.root;
        this->comparator = move(other.comparator);
        other.root = nullptr;
    }
    
    GeneralRedBlackTree& operator=(GeneralRedBlackTree&& other) noexcept {
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

	const Node* getRoot() const { return this->root; }
};

template<typename T, typename Comparator = DefaultComparator<T>, typename StackType = StaticStack<RedBlackTreeNode<T, NoAugmentation>**, 64>>
using RedBlackTree = GeneralRedBlackTree<T, NoAugmentation, Comparator, StackType>;

// Helper struct to package augmentation info for AugmentedRedBlackTree
template<typename AugData, typename AugAccumulator>
struct AugmentationPackage {
    using Data = AugData;
    using Accumulator = AugAccumulator;
};

// Convenience alias for augmented red-black trees
template<typename T, typename AugData, typename AugAccumulator, typename Comparator = DefaultComparator<T>, typename StackType = StaticStack<RedBlackTreeNode<T, AugmentationPackage<AugData, AugAccumulator>>**, 64>>
using AugmentedRedBlackTree = GeneralRedBlackTree<T, AugmentationPackage<AugData, AugAccumulator>, Comparator, StackType>;

#endif //TREES_H
