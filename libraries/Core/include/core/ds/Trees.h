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

template<typename NodeType, typename BinaryTreeInfoExtractor>
concept BinaryTreeNodeType = requires(NodeType &node)
{
	{ BinaryTreeInfoExtractor::left(node) } -> IsSame<NodeType *&>;
	{ BinaryTreeInfoExtractor::right(node) } -> IsSame<NodeType *&>;
	{ BinaryTreeInfoExtractor::data(node) } -> IsDifferent<void>;
};

template<typename NodeType, typename RedBlackTreeInfoExtractor>
concept RedBlackTreeNodeType = requires(NodeType &node, const RedBlackTreeInfoExtractor extractor, bool b)
{
	{ RedBlackTreeInfoExtractor::isRed(node) } -> convertible_to<bool>;
	{ RedBlackTreeInfoExtractor::setRed(node, b) } -> IsSame<void>;
	requires BinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;
};

template<typename NodeType, typename BinaryTreeInfoExtractor>
concept AugmentedBinaryTreeNodeType = requires(NodeType &node, const NodeType &cnode,
                                               const BinaryTreeInfoExtractor extractor)
{
	requires IsReference<decltype(BinaryTreeInfoExtractor::augmentedData(node))>;
	{
		BinaryTreeInfoExtractor::recomputeAugmentedData(cnode, &cnode, &cnode)
	} -> IsSame<remove_reference_t<decltype(BinaryTreeInfoExtractor::augmentedData(node))> >;
};

template<typename NodeType, typename BinaryTreeInfoExtractor>
concept ParentPointerBinaryTreeNodeType = requires(NodeType &node, const BinaryTreeInfoExtractor extractor)
{
	{ BinaryTreeInfoExtractor::parent(node) } -> IsSame<NodeType *&>;
};

struct NoAugmentation {
};

enum TreeSearchAction {
	Continue,
	Stop
};

template<typename NodeType, typename Visitor>
concept TreeVisitor = requires(NodeType &node, Visitor &&visitor)
{
	{ visitor(node) } -> IsSame<TreeSearchAction>;
};

template<typename NodeType, typename Visitor>
concept ConstTreeVisitor = requires(const NodeType &node, Visitor &&visitor)
{
	{ visitor(node) } -> IsSame<TreeSearchAction>;
};

template<typename NodeType, typename BinaryTreeInfoExtractor>
	requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>
class IntrusiveBinaryTree {
	static_assert(!requires(NodeType& n) { BinaryTreeInfoExtractor::augmentedData(n); } ||
			  AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>,
			  "Extractor has augmentedData method but doesn't satisfy AugmentedBinaryTreeNodeType concept. "
			  "Common cause: recomputeAugmentedData has wrong signature - should take const references.");
protected:
	template<typename Visitor>
	class VisitorWrapper {
		Visitor &&visitor;



	public:
		VisitorWrapper(Visitor &&v) : visitor(forward<Visitor>(v)) {
		}

		TreeSearchAction operator()(NodeType &node) {
			if constexpr (is_same_v<decltype(visitor(node)), void>) {
				visitor(node);
				return Continue;
			} else {
				static_assert(is_same_v<decltype(visitor(node)), TreeSearchAction>);
				return visitor(node);
			}
		}
	};

	template<typename Visitor>
	class ConstVisitorWrapper {
		Visitor &&visitor;

	public:
		ConstVisitorWrapper(Visitor &&v) : visitor(forward<Visitor>(v)) {
		}

		TreeSearchAction operator()(const NodeType &node) {
			if constexpr (is_same_v<decltype(visitor(node)), void>) {
				visitor(node);
				return Continue;
			} else {
				static_assert(is_same_v<decltype(visitor(node)), TreeSearchAction>);
				return visitor(node);
			}
		}
	};

	NodeType *root;

	template<typename Visitor>
		requires TreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstInOrderImpl(Visitor &&visitor, NodeType *node = nullptr) {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitor(*node);
		if (result == Stop) {
			return Stop;
		}
		return visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
	}

	template<typename Visitor>
		requires TreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstReverseOrderImpl(Visitor &&visitor, NodeType *node = nullptr) {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitor(*node);
		if (result == Stop) {
			return Stop;
		}
		return visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
	}

	template<typename Visitor>
		requires TreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstPostOrderImpl(Visitor &&visitor, NodeType *node = nullptr) {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
		if (result == Stop) {
			return Stop;
		}
		return visitor(*node);
	}

	template<typename Visitor>
		requires ConstTreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstInOrderImpl(Visitor &&visitor, const NodeType *node = nullptr) const {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitor(*node);
		if (result == Stop) {
			return Stop;
		}
		return visitDepthFirstInOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
	}

	template<typename Visitor>
		requires ConstTreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstReverseOrderImpl(Visitor &&visitor, const NodeType *node = nullptr) const {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitor(*node);
		if (result == Stop) {
			return Stop;
		}
		return visitDepthFirstReverseOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
	}

	template<typename Visitor>
		requires ConstTreeVisitor<NodeType, Visitor>
	TreeSearchAction visitDepthFirstPostOrderImpl(Visitor &&visitor, const NodeType *node = nullptr) const {
		if (node == nullptr) {
			return Continue;
		}
		auto result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::left(*node));
		if (result == Stop) {
			return Stop;
		}
		result = visitDepthFirstPostOrderImpl(forward<Visitor>(visitor), BinaryTreeInfoExtractor::right(*node));
		if (result == Stop) {
			return Stop;
		}
		return visitor(*node);
	}

	bool rotateLeft(NodeType *&node) {
		if (node == nullptr) {
			return false;
		}
		if (BinaryTreeInfoExtractor::right(*node) == nullptr) {
			return false;
		}
		NodeType *pivot = node;
		NodeType *newRoot = BinaryTreeInfoExtractor::right(*node);
		//Pivot's right child becomes newRoot's left child
		BinaryTreeInfoExtractor::right(*pivot) = BinaryTreeInfoExtractor::left(*newRoot);
		//NewRoot's left child becomes pivot
		BinaryTreeInfoExtractor::left(*newRoot) = pivot;
		//Update the root.
		node = newRoot;
		if constexpr (ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			BinaryTreeInfoExtractor::parent(*newRoot) = BinaryTreeInfoExtractor::parent(*pivot);
			BinaryTreeInfoExtractor::parent(*pivot) = newRoot;
			auto *child = BinaryTreeInfoExtractor::right(*pivot);
			if (child != nullptr) {
				BinaryTreeInfoExtractor::parent(*child) = pivot;
			}
		}
		if constexpr (AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
					*pivot, BinaryTreeInfoExtractor::left(*pivot), BinaryTreeInfoExtractor::right(*pivot));
				BinaryTreeInfoExtractor::augmentedData(*pivot) = recomputedAugmentedData;
			} {
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
					*newRoot, BinaryTreeInfoExtractor::left(*newRoot), BinaryTreeInfoExtractor::right(*newRoot));
				BinaryTreeInfoExtractor::augmentedData(*newRoot) = recomputedAugmentedData;
			}
		}
		return true;
	}

	bool rotateRight(NodeType *&node) {
		if (node == nullptr) {
			return false;
		}
		if (BinaryTreeInfoExtractor::left(*node) == nullptr) {
			return false;
		}
		NodeType *pivot = node;
		NodeType *newRoot = BinaryTreeInfoExtractor::left(*node);
		//Pivot's left child becomes newRoot's right child
		BinaryTreeInfoExtractor::left(*pivot) = BinaryTreeInfoExtractor::right(*newRoot);
		//NewRoot's right child becomes pivot
		BinaryTreeInfoExtractor::right(*newRoot) = pivot;
		//Update the root
		node = newRoot;
		if constexpr (ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			BinaryTreeInfoExtractor::parent(*newRoot) = BinaryTreeInfoExtractor::parent(*pivot);
			BinaryTreeInfoExtractor::parent(*pivot) = newRoot;
			auto *child = BinaryTreeInfoExtractor::left(*pivot);
			if (child != nullptr) {
				BinaryTreeInfoExtractor::parent(*child) = pivot;
			}
		}
		if constexpr (AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>) {
			{
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
					*pivot, BinaryTreeInfoExtractor::left(*pivot), BinaryTreeInfoExtractor::right(*pivot));
				BinaryTreeInfoExtractor::augmentedData(*pivot) = recomputedAugmentedData;
			} {
				auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
					*newRoot, BinaryTreeInfoExtractor::left(*newRoot), BinaryTreeInfoExtractor::right(*newRoot));
				BinaryTreeInfoExtractor::augmentedData(*newRoot) = recomputedAugmentedData;
			}
		}
		return true;
	}

	bool verifyAugmentationData(NodeType *node) const
		requires AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor> {
		if (node == nullptr) {
			return true;
		}
		bool leftValid = verifyAugmentationData(BinaryTreeInfoExtractor::left(*node));
		bool rightValid = verifyAugmentationData(BinaryTreeInfoExtractor::right(*node));
		if (!leftValid || !rightValid) {
			return false;
		}
		auto augmentedData = BinaryTreeInfoExtractor::augmentedData(*node);
		auto computedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
			*node, BinaryTreeInfoExtractor::left(*node), BinaryTreeInfoExtractor::right(*node));
		return augmentedData == computedAugmentedData;
	}

	bool verifyParentPointers(NodeType *node) const requires ParentPointerBinaryTreeNodeType<NodeType,
		BinaryTreeInfoExtractor> {
		if (node == nullptr) { return true; }
		if (node == this->root) { return true; }
		NodeType *parent = BinaryTreeInfoExtractor::parent(*node);
		if (parent == nullptr) { return false; }
		bool isLeftChild = BinaryTreeInfoExtractor::left(*parent) == node;
		bool isRightChild = BinaryTreeInfoExtractor::right(*parent) == node;
		if (!isLeftChild && !isRightChild) { return false; }

		bool leftValid = verifyParentPointers(BinaryTreeInfoExtractor::left(*node));
		bool rightValid = verifyParentPointers(BinaryTreeInfoExtractor::right(*node));
		return leftValid && rightValid;
	}

	bool verifyParentPointers() const requires ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor> {
		return verifyParentPointers(this->root);
	}

public:
	IntrusiveBinaryTree() : root(nullptr) {
	}

	explicit IntrusiveBinaryTree(NodeType *r) : root(r) {
	}

	template<typename Visitor>
	void visitDepthFirstInOrder(Visitor &&visitor) {
		visitDepthFirstInOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstInOrder(Visitor &&visitor, NodeType *start) {
		visitDepthFirstInOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
	}

	template<typename Visitor>
	void visitDepthFirstReverseOrder(Visitor &&visitor) {
		visitDepthFirstReverseOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstReverseOrder(Visitor &&visitor, NodeType *start) {
		visitDepthFirstReverseOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
	}

	template<typename Visitor>
	void visitDepthFirstPostOrder(Visitor &&visitor) {
		visitDepthFirstPostOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstPostOrder(Visitor &&visitor, NodeType *start) {
		visitDepthFirstPostOrderImpl(VisitorWrapper(forward<Visitor>(visitor)), start);
	}

	template<typename Visitor>
	void visitDepthFirstInOrder(Visitor &&visitor) const {
		visitDepthFirstInOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstInOrder(Visitor &&visitor, const NodeType *start) const {
		visitDepthFirstInOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
	}

	template<typename Visitor>
	void visitDepthFirstReverseOrder(Visitor &&visitor) const {
		visitDepthFirstReverseOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstReverseOrder(Visitor &&visitor, const NodeType *start) const {
		visitDepthFirstReverseOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
	}

	template<typename Visitor>
	void visitDepthFirstPostOrder(Visitor &&visitor) const {
		visitDepthFirstPostOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), root);
	}

	template<typename Visitor>
	void visitDepthFirstPostOrder(Visitor &&visitor, const NodeType *start) const {
		visitDepthFirstPostOrderImpl(ConstVisitorWrapper(forward<Visitor>(visitor)), start);
	}

	//Intentionally does not delete an overwritten child, since this is intrusive and meant to support use in LibAlloc
	void setLeftChild(NodeType *parent, NodeType *child) {
		if (parent != nullptr) {
			BinaryTreeInfoExtractor::left(*parent) = child;
		}
	}

	void setRightChild(NodeType *parent, NodeType *child) {
		if (parent != nullptr) {
			BinaryTreeInfoExtractor::right(*parent) = child;
		}
	}

	NodeType *getRoot() {
		return root;
	}
};

template<typename NodeType, typename BinaryTreeInfoExtractor>
	requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>
using DefaultBinaryTreeNodeComparator = DefaultComparator<remove_reference_t<decltype(BinaryTreeInfoExtractor::data(
	declval<NodeType &>()))> >;

template<typename NodeType, typename BinaryTreeInfoExtractor, typename Comparator>
concept BinaryTreeComparator = requires(const NodeType &node1, const NodeType &node2, Comparator &comparator)
{
	{ comparator(BinaryTreeInfoExtractor::data(node1), BinaryTreeInfoExtractor::data(node2)) } -> convertible_to<bool>;
};

template<typename NodeType, typename BinaryTreeInfoExtractor, typename Comparator=DefaultBinaryTreeNodeComparator<
	NodeType, BinaryTreeInfoExtractor> >
	requires BinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor> && BinaryTreeComparator<NodeType,
		         BinaryTreeInfoExtractor, Comparator>
class IntrusiveBinarySearchTree : protected IntrusiveBinaryTree<NodeType, BinaryTreeInfoExtractor> {
	using Parent = IntrusiveBinaryTree<NodeType, BinaryTreeInfoExtractor>;
	using NodeDataWithReference = decltype(BinaryTreeInfoExtractor::data(declval<NodeType &>()));
	using NodeData = remove_reference_t<NodeDataWithReference>;
	static_assert(!requires(NodeType& n) { BinaryTreeInfoExtractor::augmentedData(n); } ||
		  AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>,
		  "Extractor has augmentedData method but doesn't satisfy AugmentedBinaryTreeNodeType concept. "
		  "Common cause: recomputeAugmentedData has wrong signature - should take const references.");

	static constexpr bool HasParentPointer = ParentPointerBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>;
	static constexpr bool AugmentedNode = AugmentedBinaryTreeNodeType<NodeType, BinaryTreeInfoExtractor>;

protected:
	//Annoying nonsense in case multiple nodes have the same data
	NodeType **findParentPointerTreeTraverse(const NodeType *node, NodeType **searchRoot) const requires (!
		HasParentPointer) {
		if (searchRoot == nullptr) { return nullptr; }
		if (*searchRoot == node) { return searchRoot; }
		if (BinaryTreeInfoExtractor::data(*node) != BinaryTreeInfoExtractor::data(**searchRoot)) { return nullptr; }
		NodeType **possible = findParentPointerTreeTraverse(node, BinaryTreeInfoExtractor::left(**searchRoot));
		if (possible != nullptr) { return possible; }
		return findParentPointerTreeTraverse(node, BinaryTreeInfoExtractor::right(**searchRoot));
	}

	NodeType **findParentPointer(const NodeType *node) const requires (!HasParentPointer) {
		NodeType **current = &this->root;
		if (node == nullptr) { return nullptr; }
		NodeDataWithReference value = BinaryTreeInfoExtractor::data(*node);
		while (*current != nullptr) {
			NodeDataWithReference currentValue = BinaryTreeInfoExtractor::data(**current);
			if (value == currentValue) {
				return findParentPointerTreeTraverse(node, current);
			} else if (comparator(value, currentValue)) {
				current = &BinaryTreeInfoExtractor::right(**current);
			} else {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
		}
		return nullptr;
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeData &targetValue, StackType &ancestryStack) requires (!HasParentPointer) {
		NodeType **current = &this->root;
		ancestryStack.push(current);
		while (*current != nullptr) {
			NodeData &value = BinaryTreeInfoExtractor::data(**current);
			if (targetValue == value) { return true; }
			//if value < targetValue, go right
			if (comparator(value, targetValue)) {
				current = &BinaryTreeInfoExtractor::right(**current);
			} else {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(current);
		}
		return false;
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	//Populates the ancestry stack starting at node. *ancestryStack[-1] is either if the stack is empty,
	//points to the would-be parent after a BST insert if the node does not exist
	//or points to the node containing targetValue
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeData &targetValue, StackType &ancestryStack) requires (!HasParentPointer) {
		NodeType *current = this->root;
		while (current != nullptr) {
			ancestryStack.push(current);
			NodeData &value = BinaryTreeInfoExtractor::data(*current);
			if (targetValue == value) { return true; }
			if (comparator(value, targetValue)) {
				current = BinaryTreeInfoExtractor::right(*current);
			} else {
				current = BinaryTreeInfoExtractor::left(*current);
			}
		}
		return false;
	}


	//More annoying nonsense in case multiple nodes have the same data
	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	bool tryFindTargetNode(NodeType *targetNode, NodeType *searchNode, StackType &ancestryStack) requires (!
		HasParentPointer) {
		if (searchNode == nullptr) { return false; }
		if (searchNode == targetNode) { return true; }
		if (BinaryTreeInfoExtractor::data(*targetNode) != BinaryTreeInfoExtractor::data(*searchNode)) { return false; }
		ancestryStack.push(&BinaryTreeInfoExtractor::left(*searchNode));
		bool found = tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::left(*searchNode), ancestryStack);
		if (found) { return true; }
		ancestryStack.pop();
		ancestryStack.push(&BinaryTreeInfoExtractor::right(*searchNode));
		return tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::right(*searchNode), ancestryStack);
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	bool tryFindTargetNode(NodeType *targetNode, NodeType *searchNode, StackType &ancestryStack) requires (!
		HasParentPointer) {
		if (searchNode == nullptr) { return false; }
		if (searchNode == targetNode) { return true; }
		if (BinaryTreeInfoExtractor::data(*targetNode) != BinaryTreeInfoExtractor::data(*searchNode)) { return false; }
		ancestryStack.push(BinaryTreeInfoExtractor::left(*searchNode));
		bool found = tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::left(*searchNode), ancestryStack);
		if (found) { return true; }
		ancestryStack.pop();
		ancestryStack.push(BinaryTreeInfoExtractor::right(*searchNode));
		return tryFindTargetNode(targetNode, BinaryTreeInfoExtractor::right(*searchNode), ancestryStack);
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeType *targetNode, StackType &ancestryStack) requires (!HasParentPointer) {
		NodeType **current = &this->root;
		ancestryStack.push(current);
		const NodeData &targetValue = BinaryTreeInfoExtractor::data(*targetNode);
		while (*current != nullptr) {
			NodeData &value = BinaryTreeInfoExtractor::data(**current);
			if (targetValue == value) {
				return tryFindTargetNode(targetNode, *current, ancestryStack);
			}
			//if value < targetValue, go right
			if (comparator(value, targetValue)) {
				current = &BinaryTreeInfoExtractor::right(**current);
			} else {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(current);
		}
		return false;
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	//Populates the ancestry stack starting at &root. *ancestryStack[-1] is either nullptr if no node exists
	//with value targetValue, or it points to a node with that value
	//Returns true if the node exists, and false otherwise.
	bool populateAncestryStack(const NodeType *targetNode, StackType &ancestryStack) requires (!HasParentPointer) {
		NodeType **current = &this->root;
		ancestryStack.push(*current);
		const NodeData &targetValue = BinaryTreeInfoExtractor::data(*targetNode);
		while (*current != nullptr) {
			NodeData &value = BinaryTreeInfoExtractor::data(**current);
			if (targetValue == value) {
				return tryFindTargetNode(targetNode, *current, ancestryStack);
			}
			//if value < targetValue, go right
			if (comparator(value, targetValue)) {
				current = &BinaryTreeInfoExtractor::right(**current);
			} else {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			ancestryStack.push(*current);
		}
		return false;
	}

	bool updateNodeAugmentationData(NodeType *node) requires AugmentedBinaryTreeNodeType<NodeType,
		BinaryTreeInfoExtractor> {
		if (node == nullptr) { return false; }
		auto oldData = BinaryTreeInfoExtractor::augmentedData(*node);
		auto recomputedAugmentedData = BinaryTreeInfoExtractor::recomputeAugmentedData(
			*node, BinaryTreeInfoExtractor::left(*node), BinaryTreeInfoExtractor::right(*node));
		BinaryTreeInfoExtractor::augmentedData(*node) = recomputedAugmentedData;
		return oldData == recomputedAugmentedData;
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	void fixupAugmentationData(StackType &ancestryStack) requires AugmentedNode && (!HasParentPointer) {
		while (!ancestryStack.empty()) {
			updateNodeAugmentationData(ancestryStack.pop());
		}
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	void fixupAugmentationData(StackType &ancestryStack) requires AugmentedNode && (!HasParentPointer) {
		while (!ancestryStack.empty()) {
			updateNodeAugmentationData(*ancestryStack.pop());
		}
	}

	void fixupAugmentationData(NodeType *node) requires AugmentedNode && HasParentPointer {
		if (this->root == nullptr) { return; }

		NodeType *current = node;

		do {
			updateNodeAugmentationData(current);
		} while ((current = BinaryTreeInfoExtractor::parent(*current)));
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	void propagateAugmentationRefresh(StackType &ancestryStack) requires AugmentedNode && (!HasParentPointer) {
		while (!ancestryStack.empty()) {
			if (updateNodeAugmentationData(ancestryStack.pop())) return;
		}
	}

	void propagateAugmentationRefresh(NodeType &node) requires AugmentedNode && HasParentPointer {
		NodeType *current = &node;
		do {
			if (updateNodeAugmentationData(current)) return;
		} while ((current = BinaryTreeInfoExtractor::parent(*current)));
	}

	Comparator comparator;

	void insert(NodeType *toInsert, NodeType *&root) requires (!AugmentedNode || HasParentPointer) {
		NodeType *parent = nullptr;
		NodeType **current = &root;

		while (*current != nullptr) {
			parent = *current;
			if (comparator(BinaryTreeInfoExtractor::data(*toInsert), BinaryTreeInfoExtractor::data(**current))) {
				current = &BinaryTreeInfoExtractor::left(**current);
			} else {
				current = &BinaryTreeInfoExtractor::right(**current);
			}
		}
		*current = toInsert;

		//Make sure to set the children to null just in case there's any stale data in the node
		BinaryTreeInfoExtractor::left(*toInsert) = nullptr;
		BinaryTreeInfoExtractor::right(*toInsert) = nullptr;

		if constexpr (HasParentPointer) {
			BinaryTreeInfoExtractor::parent(*toInsert) = parent;
			if constexpr (AugmentedNode) {
				propagateAugmentationRefresh(*toInsert);
			}
		}
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	void insert(NodeType *toInsert, NodeType *&root) requires (AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
		NodeType **current = &root;

		while (*current != nullptr) {
			ancestryStack.push(*current);
			if (comparator(BinaryTreeInfoExtractor::data(*toInsert), BinaryTreeInfoExtractor::data(**current))) {
				current = &BinaryTreeInfoExtractor::left(**current);
			} else {
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

	NodeType **findImpl(const remove_reference_t<NodeData> &value, NodeType **root) const {
		NodeType **current = root;
		while (*current != nullptr) {
			if (value == BinaryTreeInfoExtractor::data(**current)) {
				return current;
			}
			//if value < current, go left
			if (comparator(value, BinaryTreeInfoExtractor::data(**current))) {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			//otherwise go right
			else {
				current = &BinaryTreeInfoExtractor::right(**current);
			}
		}
		return nullptr;
	}

	const NodeType *const*findImpl(const remove_reference_t<NodeData> &value, const NodeType *const*root) const {
		const NodeType *const*current = root;
		while (*current != nullptr) {
			if (value == BinaryTreeInfoExtractor::data(**current)) {
				return current;
			}
			//if value < current, go left
			if (comparator(value, BinaryTreeInfoExtractor::data(**current))) {
				current = &BinaryTreeInfoExtractor::left(**current);
			}
			//otherwise go right
			else {
				current = &BinaryTreeInfoExtractor::right(**current);
			}
		}
		return nullptr;
	}

	NodeType *eraseNodeImpl(NodeType **toRemove) requires (!AugmentedNode || HasParentPointer) {
		NodeType *toReturn = *toRemove;
		//If one of the children of the node we are trying to erase is null, deletion is simple: just replace
		//the pointer to toRemove in its parent with the other child
		NodeType *parent = nullptr;
		if (BinaryTreeInfoExtractor::left(*(*toRemove)) == nullptr) {
			if constexpr (HasParentPointer) {
				parent = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}
			*toRemove = BinaryTreeInfoExtractor::right(*(*toRemove));
			//It's possible toRemove is a leaf in this case
			if constexpr (HasParentPointer) {
				if (*toRemove != nullptr) {
					BinaryTreeInfoExtractor::parent(*(*toRemove)) = parent;
				}
			}
			if constexpr (AugmentedNode) {
				fixupAugmentationData(parent);
			}
		} else if (BinaryTreeInfoExtractor::right(*(*toRemove)) == nullptr) {
			if constexpr (HasParentPointer) {
				parent = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}
			*toRemove = BinaryTreeInfoExtractor::left(*(*toRemove));
			//But here, we know toRemove's left child is not null, so no need for a null check
			if constexpr (HasParentPointer) {
				BinaryTreeInfoExtractor::parent(*(*toRemove)) = parent;
			}
			if constexpr (AugmentedNode) {
				fixupAugmentationData(parent);
			}
		}
		//Otherwise, we need to find the successor of toRemove, and replace it with the successor
		//Notably the successor is not toRemove's immediate parent since BinaryTreeInfoExtractor::right is not null
		else {
			//Find the successor of toRemove
			NodeType **successor = &BinaryTreeInfoExtractor::right(*(*toRemove));
			bool immediateChild = true;
			while (BinaryTreeInfoExtractor::left(*(*successor)) != nullptr) {
				immediateChild = false;
				successor = &BinaryTreeInfoExtractor::left(*(*successor));
			}

			if (immediateChild) {
				auto leftChild = BinaryTreeInfoExtractor::left(**toRemove);
				BinaryTreeInfoExtractor::left(**successor) = leftChild;

				if constexpr (HasParentPointer) {
					if (leftChild != nullptr) {
						BinaryTreeInfoExtractor::parent(*leftChild) = *successor;
					}
					BinaryTreeInfoExtractor::parent(**successor) = BinaryTreeInfoExtractor::parent(**toRemove);
				}
				*toRemove = *successor;
				if constexpr (AugmentedNode) {
					fixupAugmentationData(*toRemove);
				}
				return toReturn;
			}
			//In this case, we know successor is not the immediate child of toReplace.
			//Retain a pointer to the successor, as we will be replacing it with its right child
			NodeType *successorPtr = *successor;

			if constexpr (HasParentPointer) {
				parent = BinaryTreeInfoExtractor::parent(*successorPtr);
			}
			//Remove the successor from the tree, replace it with its right child. The left will always be empty
			*successor = BinaryTreeInfoExtractor::right(*successorPtr);
			if constexpr (HasParentPointer) {
				if (*successor != nullptr) {
					BinaryTreeInfoExtractor::parent(*(*successor)) = parent;
				}
				BinaryTreeInfoExtractor::parent(*successorPtr) = BinaryTreeInfoExtractor::parent(*(*toRemove));
			}

			//Now copy the child pointers from toRemove to the successor
			BinaryTreeInfoExtractor::left(*successorPtr) = BinaryTreeInfoExtractor::left(*(*toRemove));
			BinaryTreeInfoExtractor::right(*successorPtr) = BinaryTreeInfoExtractor::right(*(*toRemove));

			if constexpr (HasParentPointer) {
				NodeType *left = BinaryTreeInfoExtractor::left(*successorPtr);
				NodeType *right = BinaryTreeInfoExtractor::right(*successorPtr);
				if (left != nullptr) {
					BinaryTreeInfoExtractor::parent(*left) = successorPtr;
				}
				if (right != nullptr) {
					BinaryTreeInfoExtractor::parent(*right) = successorPtr;
				}
			}

			//update the parent pointer toRemove to point to its successor
			*toRemove = successorPtr;

			if constexpr (AugmentedNode) {
				fixupAugmentationData(parent);
			}
		}
		return toReturn;
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	NodeType *eraseNodeImpl(StackType &ancestryStack) requires (AugmentedNode && !HasParentPointer) {
		if (ancestryStack.empty()) { return nullptr; }
		NodeType **toRemove = ancestryStack[-1];
		NodeType *toReturn = *toRemove;
		//If the left child is empty, just replace toRemove with its right child
		if (BinaryTreeInfoExtractor::left(*(*toRemove)) == nullptr) {
			*toRemove = BinaryTreeInfoExtractor::right(*(*toRemove));
			fixupAugmentationData(ancestryStack);
		} else if (BinaryTreeInfoExtractor::right(*(*toRemove)) == nullptr) {
			*toRemove = BinaryTreeInfoExtractor::left(*(*toRemove));
			fixupAugmentationData(ancestryStack);
		} else {
			NodeType **successor = &BinaryTreeInfoExtractor::right(*(*toRemove));
			ancestryStack.push(successor);
			bool immediateChild = true;
			while (BinaryTreeInfoExtractor::left(*(*successor)) != nullptr) {
				immediateChild = false;
				successor = &BinaryTreeInfoExtractor::left(*(*successor));
				ancestryStack.push(successor);
			}
			if (immediateChild) {
				BinaryTreeInfoExtractor::left(**successor) = BinaryTreeInfoExtractor::left(**toRemove);
				*toRemove = *successor;
				fixupAugmentationData(ancestryStack);
				return toReturn;
			}

			NodeType *successorPtr = *successor;
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
	NodeType *eraseImpl(const remove_reference_t<NodeData> &value, NodeType *&root) requires (
		!AugmentedNode || HasParentPointer) {
		NodeType **toRemove = this->findImpl(value, &root);
		if (toRemove == nullptr) { return nullptr; }
		return eraseNodeImpl(toRemove);
	}

	//Returns a pointer to the erased type - in an intrusive environment, it is up to the caller to delete the memory
	//as appropriate
	NodeType *eraseImpl(const NodeType *node, NodeType *&root) requires (!AugmentedNode || HasParentPointer) {
		NodeType **toRemove = this->findParentPointerTreeTraverse(node, &root);
		if (toRemove == nullptr) { return nullptr; }
		return eraseNodeImpl(toRemove);
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	NodeType *eraseImpl(const remove_reference_t<NodeData> &value, NodeType *&root) requires (
		AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
		if (populateAncestryStack(value, ancestryStack)) {
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	NodeType *eraseImpl(const remove_reference_t<NodeData> &value, NodeType *&root) requires (
		AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
		if (populateAncestryStack(value, ancestryStack)) {
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	NodeType *eraseImpl(const NodeType *node, NodeType *&root) requires (AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
		if (populateAncestryStack(node, ancestryStack)) {
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
	}

	template<typename StackType>
		requires IsStack<NodeType *, StackType>
	NodeType *eraseImpl(const NodeType *node, NodeType *&root) requires (AugmentedNode && !HasParentPointer) {
		StackType ancestryStack;
		if (populateAncestryStack(const_cast<NodeType *>(node), ancestryStack)) {
			return eraseNodeImpl(ancestryStack);
		}
		return nullptr;
	}

	NodeType *const&floorImpl(const remove_reference_t<NodeData> &value) const {
		static NodeType *const invalid_node = nullptr;
		NodeType *const *result = &invalid_node;
		NodeType *const *current = &(this->root);

		while (*current != nullptr) {
			if (BinaryTreeInfoExtractor::data(*(*current)) == value) {
				return *current; // Exact match
			}
			if (comparator(BinaryTreeInfoExtractor::data(*(*current)), value)) {
				// current < value, so current is a candidate
				result = current;
				current = &BinaryTreeInfoExtractor::right(*(*current));
			} else {
				// current > value, go left
				current = &BinaryTreeInfoExtractor::left(*(*current));
			}
		}
		return *result;
	}

	NodeType *ceilImpl(const remove_reference_t<NodeData> &value) const {
		static NodeType *const invalid_node = nullptr;
		NodeType *const*result = &invalid_node;
		NodeType *const*current = &(this->root);

		while (*current != nullptr) {
			if (BinaryTreeInfoExtractor::data(*(*current)) == value) {
				return *current; // Exact match
			} else if (comparator(value, BinaryTreeInfoExtractor::data(*(*current)))) {
				// value < current, so current is a candidate
				result = current;
				current = &BinaryTreeInfoExtractor::left(*(*current));
			} else {
				// value > current, go right
				current = &BinaryTreeInfoExtractor::right(*(*current));
			}
		}
		return *result;
	}

	NodeType *successorImpl(const NodeType *node) const {
		static NodeType *const invalid_node = nullptr;
		if (node == nullptr) return invalid_node;

		// Case 1: node has right child
		if (BinaryTreeInfoExtractor::right(*node) != nullptr) {
			NodeType *const*current = &BinaryTreeInfoExtractor::right(*node);
			while (BinaryTreeInfoExtractor::left(*(*current)) != nullptr) {
				current = &BinaryTreeInfoExtractor::left(*(*current));
			}
			return *current;
		}

		// Case 2: no right child, find ancestor where node is in left subtree
		NodeType *const*successor = &invalid_node;
		NodeType *const*current = &(this->root);

		while (*current != nullptr) {
			if (comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*(*current)))) {
				successor = current; // current > node, so it's a candidate
				current = &BinaryTreeInfoExtractor::left(*(*current));
			} else if (comparator(BinaryTreeInfoExtractor::data(*(*current)), BinaryTreeInfoExtractor::data(*node))) {
				current = &BinaryTreeInfoExtractor::right(*(*current));
			} else {
				// Found the node, successor is already set (or null)
				break;
			}
		}
		return *successor;
	}

	NodeType *predecessorImpl(const NodeType *node) const {
		static NodeType *const invalid_node = nullptr;
		if (node == nullptr) return invalid_node;

		// Case 1: node has left child
		if (BinaryTreeInfoExtractor::left(*node) != nullptr) {
			NodeType *const*current = &BinaryTreeInfoExtractor::left(*node);
			while (BinaryTreeInfoExtractor::right(*(*current)) != nullptr) {
				current = &BinaryTreeInfoExtractor::right(*(*current));
			}
			return *current;
		}

		// Case 2: no left child, find ancestor where node is in right subtree
		NodeType *const*predecessor = &invalid_node;
		NodeType *const*current = &(this->root);

		while (*current != nullptr) {
			if (comparator(BinaryTreeInfoExtractor::data(*(*current)), BinaryTreeInfoExtractor::data(*node))) {
				predecessor = current; // current < node, so it's a candidate
				current = &BinaryTreeInfoExtractor::right(*(*current));
			} else if (comparator(BinaryTreeInfoExtractor::data(*node), BinaryTreeInfoExtractor::data(*(*current)))) {
				current = &BinaryTreeInfoExtractor::left(*(*current));
			} else {
				// Found the node, predecessor is already set (or null)
				break;
			}
		}
		return *predecessor;
	}

	template<typename T, typename F, typename TComparator>
		requires Invocable<F, T, NodeData> && Invocable<TComparator, bool, T, T>
	NodeType *mappedCeilImpl(T value, F transform) const {
		NodeType *const*result = nullptr;
		NodeType *const*current = &(this->root);
		TComparator tcomp;

		while (*current != nullptr) {
			T currentValue = transform(BinaryTreeInfoExtractor::data(*(*current)));
			if (currentValue == value) {
				return *current; // Exact match
			}
			if (tcomp(value, currentValue)) {
				// value < current, so current is a candidate
				result = current;
				current = &BinaryTreeInfoExtractor::left(*(*current));
			} else {
				// value > current, go right
				current = &BinaryTreeInfoExtractor::right(*(*current));
			}
		}
		return *result;
	}

	template<typename T, typename F, typename TComparator>
		requires Invocable<F, T, NodeData> && Invocable<TComparator, bool, T, T>
	NodeType *mappedFloorImpl(T value, F transform) const {
		NodeType *const*result = nullptr;
		NodeType *const*current = &(this->root);
		TComparator tcomp;

		while (*current != nullptr) {
			T currentValue = transform(BinaryTreeInfoExtractor::data(*(*current)));
			if (currentValue == value) {
				return *current; // Exact match
			}
			if (tcomp(currentValue, value)) {
				// current < value, so current is a candidate
				result = current;
				current = &BinaryTreeInfoExtractor::left(*(*current));
			} else {
				// current > value, go right
				current = &BinaryTreeInfoExtractor::right(*(*current));
			}
		}
		return *result;
	}

public:
	using Parent::visitDepthFirstInOrder;
	using Parent::visitDepthFirstReverseOrder;
	using Parent::visitDepthFirstPostOrder;
	using Parent::getRoot;

	void insert(NodeType *node) requires (!AugmentedNode || HasParentPointer) {
		this->insert(node, this->root);
	}

	template<typename StackType = StaticStack<NodeType *, 64> >
		requires IsStack<NodeType *, StackType>
	void insert(NodeType *node) requires (AugmentedNode && !HasParentPointer) {
		this->template insert<StackType>(node, this->root);
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	void insert(NodeType *node) requires (AugmentedNode && !HasParentPointer) {
		this->template insert<StackType>(node, this->root);
	}

	NodeType *find(const remove_reference_t<NodeData> &value) {
		NodeType **result = this->findImpl(value, &this->root);
		if (result == nullptr) { return nullptr; }
		return *result;
	}

	const NodeType *find(const remove_reference_t<NodeData> &value) const {
		const NodeType *const*result = this->findImpl(value, &this->root);
		if (result == nullptr) { return nullptr; }
		return *result;
	}

	NodeType *erase(const remove_reference_t<NodeData> &value) requires (!AugmentedNode || HasParentPointer) {
		return this->eraseImpl(value, this->root);
	}

	NodeType *erase(NodeType *value) requires (!AugmentedNode || HasParentPointer) {
		return this->eraseImpl(value, this->root);
	}

	template<typename StackType = StaticStack<NodeType *, 64> >
		requires IsStack<NodeType *, StackType>
	NodeType *erase(const remove_reference_t<NodeData> &value) requires (AugmentedNode && !HasParentPointer) {
		return this->template eraseImpl<StackType>(value, this->root);
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	NodeType *erase(const remove_reference_t<NodeData> &value) requires (AugmentedNode && !HasParentPointer) {
		return this->template eraseImpl<StackType>(value, this->root);
	}

	template<typename StackType = StaticStack<NodeType *, 64> >
		requires IsStack<NodeType *, StackType>
	NodeType *erase(NodeType *node) requires (AugmentedNode && !HasParentPointer) {
		return this->template eraseImpl<StackType>(node, this->root);
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	NodeType *erase(NodeType *node) requires (AugmentedNode && !HasParentPointer) {
		return this->template eraseImpl<StackType>(node, this->root);
	}

	const NodeType *floor(const remove_reference_t<NodeData> &value) const {
		return this->floorImpl(value);
	}

	NodeType *floor(remove_reference_t<NodeData> &value) const {
		return this->floorImpl(value);
	}

	const NodeType *ceil(const remove_reference_t<NodeData> &value) const {
		return this->ceilImpl(value);
	}

	NodeType *ceil(remove_reference_t<NodeData> &value) const {
		return this->ceilImpl(value);
	}

	template<typename T, typename F, typename TComparator = DefaultComparator<T> >
	NodeType *mappedCeil(T value, F transform) const {
		return this->template mappedCeilImpl<T, F, TComparator>(value, transform);
	}

	template<typename T, typename F, typename TComparator = DefaultComparator<T> >
	NodeType *mappedFloor(T value, F transform) const {
		return this->template mappedFloorImpl<T, F, TComparator>(value, transform);
	}

	const NodeType *successor(const NodeType *node) const {
		return this->successorImpl(node);
	}

	NodeType *successor(NodeType *node) const {
		return this->successorImpl(node);
	}

	const NodeType *predecessor(const NodeType *node) const {
		return this->predecessorImpl(node);
	}

	NodeType *predecessor(NodeType *node) const {
		return this->predecessorImpl(node);
	}

	NodeType *max() const {
		if (this->root == nullptr) {
			return nullptr;
		}
		NodeType *current = this->root;
		while (BinaryTreeInfoExtractor::right(*current) != nullptr) {
			current = BinaryTreeInfoExtractor::right(*current);
		}
		return current;
	}

	NodeType *min() const {
		if (this->root == nullptr) {
			return nullptr;
		}
		NodeType *current = this->root;
		while (BinaryTreeInfoExtractor::left(*current) != nullptr) {
			current = BinaryTreeInfoExtractor::left(*current);
		}
		return current;
	}
};

template<typename NodeType, typename RedBlackTreeInfoExtractor, typename Comparator = DefaultBinaryTreeNodeComparator<
	NodeType, RedBlackTreeInfoExtractor> >
	requires BinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor> &&
	         BinaryTreeComparator<NodeType, RedBlackTreeInfoExtractor, Comparator> &&
	         RedBlackTreeNodeType<NodeType, RedBlackTreeInfoExtractor>
class IntrusiveRedBlackTree : protected IntrusiveBinarySearchTree<NodeType, RedBlackTreeInfoExtractor, Comparator> {
	static constexpr bool HasParentPointer = ParentPointerBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;
	static constexpr bool AugmentedNode = AugmentedBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>;
	
	// If the extractor has augmentedData, it MUST satisfy the full AugmentedBinaryTreeNodeType concept
	static_assert(!requires(NodeType& n) { RedBlackTreeInfoExtractor::augmentedData(n); } || 
	              AugmentedBinaryTreeNodeType<NodeType, RedBlackTreeInfoExtractor>,
	              "Extractor has augmentedData method but doesn't satisfy AugmentedBinaryTreeNodeType concept. "
	              "Common cause: recomputeAugmentedData has wrong signature - should take const references.");

protected:
	using BSTParent = IntrusiveBinarySearchTree<NodeType, RedBlackTreeInfoExtractor, Comparator>;
	using BinTreeParent = IntrusiveBinaryTree<NodeType, RedBlackTreeInfoExtractor>;
	Comparator comparator;
	using NodeDataWithReference = decltype(RedBlackTreeInfoExtractor::data(declval<NodeType &>()));
	using NodeData = remove_reference_t<NodeDataWithReference>;

public:
	void dumpAsDot(Core::PrintStream &stream) requires Core::Printable<NodeData> {
		stream << "digraph G {\n";
		stream << "rankdir=TB;\n";
		stream << "node [fontname=\"Helvetica\", shape=circle, style=filled];\n";
		this->visitDepthFirstInOrder([&stream](NodeType &node) {
			stream << "v" << reinterpret_cast<uint64_t>(&node) << "[fillcolor=";
			stream << (!RedBlackTreeInfoExtractor::isRed(node) ? "black" : "red") << ", ";
			stream << "label=" << RedBlackTreeInfoExtractor::data(node);
			if (!RedBlackTreeInfoExtractor::isRed(node)) {
				stream << ", fontcolor=white";
			}
			stream << "];\n";
		});
		uint64_t nullCount = 0;
		this->visitDepthFirstInOrder([&stream, &nullCount](NodeType &node) {
			if (RedBlackTreeInfoExtractor::left(node) != nullptr) {
				stream << "v" << reinterpret_cast<uint64_t>(&node) <<
						" -> v" << reinterpret_cast<uint64_t>(RedBlackTreeInfoExtractor::left(node));
				stream << " [label=\"L\"];\n";
			} else {
				nullCount++;
				stream << "null" << nullCount << "[label=\"\", shape=point];\n";
				stream << "v" << reinterpret_cast<uint64_t>(&node) <<
						" -> null" << nullCount << " [label=\"L\"];\n";
			}
			if (RedBlackTreeInfoExtractor::right(node) != nullptr) {
				stream << "v" << reinterpret_cast<uint64_t>(&node) <<
						" -> v" << reinterpret_cast<uint64_t>(RedBlackTreeInfoExtractor::right(node));
				stream << " [label=\"R\"];\n";
			} else {
				nullCount++;
				stream << "null" << nullCount << "[label=\"\", shape=point];\n";
				stream << "v" << reinterpret_cast<uint64_t>(&node) <<
						" -> null" << nullCount << " [label=\"R\"];\n";
			}
		});
		stream << "}\n";
	}

protected:
	enum Direction {
		Left,
		Right
	};

	Direction opposite(Direction direction) {
		if (direction == Direction::Left) {
			return Direction::Right;
		}
		return Direction::Left;
	}

	NodeType *&getChild(NodeType &node, Direction direction) {
		if (direction == Direction::Left) {
			return RedBlackTreeInfoExtractor::left(node);
		}
		return RedBlackTreeInfoExtractor::right(node);
	}

	Direction getChildDirection(NodeType &parent, NodeType *childPtr) {
		if (RedBlackTreeInfoExtractor::left(parent) == childPtr) {
			return Direction::Left;
		}
		return Direction::Right;
	}

	bool isChild(const NodeType &parent, const NodeType *childPtr) {
		return RedBlackTreeInfoExtractor::left(parent) == childPtr || RedBlackTreeInfoExtractor::right(parent) ==
		       childPtr;
	}

	void rotateSubtree(NodeType *&root, Direction direction) requires (!HasParentPointer) {
		if (direction == Direction::Left) {
			BinTreeParent::rotateLeft(root);
		} else {
			BinTreeParent::rotateRight(root);
		}
	}

	void rotateSubtree(NodeType *root, Direction direction) requires (HasParentPointer) {
		NodeType **parentRef;
		if (RedBlackTreeInfoExtractor::parent(*root) == nullptr) {
			parentRef = &this->root;
		} else {
			NodeType &parent = *RedBlackTreeInfoExtractor::parent(*root);
			if (RedBlackTreeInfoExtractor::left(parent) == root) {
				parentRef = &RedBlackTreeInfoExtractor::left(parent);
			} else {
				parentRef = &RedBlackTreeInfoExtractor::right(parent);
			}
		}
		if (direction == Direction::Left) {
			BinTreeParent::rotateLeft(*parentRef);
		} else {
			BinTreeParent::rotateRight(*parentRef);
		}
	}

	template<typename StackType>
	void rotateAboutParent(StackType &ancestryStack, Direction direction) requires (!HasParentPointer) {
		NodeType **current = ancestryStack[-1];
		NodeType **parent = ancestryStack[-2];
		bool rotatingTowardsCurrent = (getChild(**parent, direction) == *current);
		//Perform the rotation
		rotateSubtree(*parent, direction);

		ancestryStack.pop();
		//If we're rotating towards current, its depth will go down
		if (rotatingTowardsCurrent) {
			ancestryStack.push(&getChild(**parent, direction));
			ancestryStack.push(current);
		}
	}

	enum Color {
		Red,
		Black
	};

	Color getColor(NodeType &node) {
		return RedBlackTreeInfoExtractor::isRed(node) ? Color::Red : Color::Black;
	}

	void setColor(NodeType &node, Color color) {
		RedBlackTreeInfoExtractor::setRed(node, color == Color::Red);
	}

	bool hasLeftChild(NodeType &node) {
		return RedBlackTreeInfoExtractor::left(node) != nullptr;
	}

	bool hasRightChild(NodeType &node) {
		return RedBlackTreeInfoExtractor::right(node) != nullptr;
	}

	bool hasChild(NodeType &node) {
		return hasLeftChild(node) || hasRightChild(node);
	}

	bool verifyRedBlackTree(NodeType *node, size_t &blackHeight) {
		if (node == nullptr) {
			blackHeight = 1;
			return true;
		}

		if constexpr (HasParentPointer) {
			if (node == this->root) {
				assert(RedBlackTreeInfoExtractor::parent(*node) == nullptr, "root's parent should be null");
			}
			if (hasLeftChild(*node)) {
				assert(RedBlackTreeInfoExtractor::parent(*RedBlackTreeInfoExtractor::left(*node)) == node,
				       "left child's parent should be node");
			}
			if (hasRightChild(*node)) {
				assert(RedBlackTreeInfoExtractor::parent(*RedBlackTreeInfoExtractor::right(*node)) == node,
				       "right child's parent should be node");
			}
		}

		if (getColor(*node) == Color::Red) {
			if (hasLeftChild(*node) && getColor(*RedBlackTreeInfoExtractor::left(*node)) == Color::Red) {
				assert(false, "Red violation");
				return false;
			}
			if (hasRightChild(*node) && getColor(*RedBlackTreeInfoExtractor::right(*node)) == Color::Red) {
				assert(false, "Red violation");
				return false;
			}
		}

		size_t leftBlackHeight = 0;
		size_t rightBlackHeight = 0;

		if (!verifyRedBlackTree(RedBlackTreeInfoExtractor::left(*node), leftBlackHeight) || !verifyRedBlackTree(
			    RedBlackTreeInfoExtractor::right(*node), rightBlackHeight)) {
			return false;
		}

		if (leftBlackHeight != rightBlackHeight) {
			assert(false, "Black violation");
			return false;
		}

		blackHeight = (getColor(*node) == Color::Black) ? (leftBlackHeight + 1) : leftBlackHeight;
		return true;
	}

	void verifyRedBlackTree() {
		size_t blackHeight = 0;
		assert(verifyRedBlackTree(this -> root, blackHeight), "RBT verification failed");
	}

	template<typename StackType>
	bool verifyAlmostRedBlackTreeImpl(NodeType *&node, size_t &blackHeight, const StackType &ancestryStack,
	                                  size_t &nodeCount) requires (!HasParentPointer) {
		size_t virtualBlackHeight = (&node == ancestryStack[-1] ? 2 : 1);
		size_t virtualNodeCount = (node == nullptr ? 0 : 1) + (&node == ancestryStack[-1] ? 1 : 0);

		if (node == nullptr) {
			blackHeight = virtualBlackHeight;
			nodeCount = virtualNodeCount;
			return true;
		}

		if (hasLeftChild(*node)) {
			assert(
				comparator(RedBlackTreeInfoExtractor::data(*RedBlackTreeInfoExtractor::left(*node)),
					RedBlackTreeInfoExtractor::data(*node)), "Left child is not less than parent");
		}

		if (hasRightChild(*node)) {
			assert(
				comparator(RedBlackTreeInfoExtractor::data(*node), RedBlackTreeInfoExtractor::data(*
					RedBlackTreeInfoExtractor::right(*node))), "Right child is not greater than parent");
		}

		if (getColor(*node) == Color::Red) {
			if (hasLeftChild(*node) && getColor(*RedBlackTreeInfoExtractor::left(*node)) == Color::Red) {
				return false;
			}
			if (hasRightChild(*node) && getColor(*RedBlackTreeInfoExtractor::right(*node)) == Color::Red) {
				return false;
			}
		}

		size_t leftBlackHeight = 0;
		size_t rightBlackHeight = 0;
		size_t leftNodeCount = 0;
		size_t rightNodeCount = 0;

		if (!verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::left(*node), leftBlackHeight, ancestryStack,
		                                  leftNodeCount) ||
		    !verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::right(*node), rightBlackHeight, ancestryStack,
		                                  rightNodeCount)) {
			return false;
		}

		if (leftBlackHeight != rightBlackHeight) {
			return false;
		}

		blackHeight = (getColor(*node) == Color::Black) ? (leftBlackHeight + virtualBlackHeight) : leftBlackHeight;
		nodeCount = leftNodeCount + rightNodeCount + virtualNodeCount;
		return true;
	}

	template<typename StackType>
	void verifyAlmostRedBlackTree(const StackType &ancestryStack) requires (!HasParentPointer) {
		size_t blackHeight = 0;
		size_t nodeCount = 0;
		for (size_t i = 0; i < ancestryStack.size() - 1; i++) {
			assert(ancestryStack[i] != ancestryStack[i + 1],
			       "verifyAlmostRedBlackTree - Ancestry stack has duplicate entry");
			assert(ancestryStack[i] != nullptr,
			       "verifyAlmostRedBlackTree - Ancestry stack has null entry that isn't top");
			assert(isChild(**ancestryStack[i], *ancestryStack[i + 1]),
			       "verifyAlmostRedBlackTree - Ancestry stack is invalid");
		}
		assert(verifyAlmostRedBlackTreeImpl(this -> root, blackHeight, ancestryStack, nodeCount),
		       "Almost RBT verification failed");
	}

	bool verifyAlmostRedBlackTreeImpl(NodeType *node, NodeType *fixupStart, Direction dir, size_t &blackHeight) requires
		(HasParentPointer) {
		if (node == nullptr) {
			blackHeight = 1;
			return true;
		}

		if (node == this->root) {
			assert(RedBlackTreeInfoExtractor::parent(*node) == nullptr, "Root has parent");
		}

		if (hasLeftChild(*node)) {
			assert(RedBlackTreeInfoExtractor::parent(*RedBlackTreeInfoExtractor::left(*node)) == node,
			       "Left child parent is incorrect");
		}
		if (hasRightChild(*node)) {
			assert(RedBlackTreeInfoExtractor::parent(*RedBlackTreeInfoExtractor::right(*node)) == node,
			       "Right child parent is incorrect");
		}

		if (getColor(*node) == Color::Red) {
			if (hasLeftChild(*node) && getColor(*RedBlackTreeInfoExtractor::left(*node)) == Color::Red) {
				assert(false, "Almost RBT - Red violation");
				return false;
			}
			if (hasRightChild(*node) && getColor(*RedBlackTreeInfoExtractor::right(*node)) == Color::Red) {
				assert(false, "Almost RBT - Red violation");
				return false;
			}
		}

		size_t leftBlackHeight = 0;
		size_t rightBlackHeight = 0;

		if (!verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::left(*node), fixupStart, dir, leftBlackHeight) ||
		    !verifyAlmostRedBlackTreeImpl(RedBlackTreeInfoExtractor::right(*node), fixupStart, dir, rightBlackHeight)) {
			assert(false, "Almost RBT - Subtree verification failed");
			return false;
		}

		if (node == fixupStart) {
			if (dir == Direction::Left) { leftBlackHeight++; } else { rightBlackHeight++; }
		}

		if (leftBlackHeight != rightBlackHeight) {
			assert(false, "Almost RBT - Black violation");
			return false;
		}

		blackHeight = (getColor(*node) == Color::Black) ? (leftBlackHeight + 1) : leftBlackHeight;
		return true;
	}

	bool verifyAlmostRedBlackTree(NodeType *fixupStart, Direction dir) requires (HasParentPointer) {
		size_t blackHeight = 0;
		return verifyAlmostRedBlackTreeImpl(this->root, fixupStart, dir, blackHeight);
	}

	size_t getTreeSize(NodeType *node) {
		if (node == nullptr) {
			return 0;
		}
		return 1 + getTreeSize(RedBlackTreeInfoExtractor::left(*node)) + getTreeSize(
			       RedBlackTreeInfoExtractor::right(*node));
	}

	size_t getTreeSize() {
		return getTreeSize(this->root);
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	//In this case, *ancestryStack[-1] will always be nullptr, pointing to the node that was just deleted.
	//Then *ancestryStack[-2] points to the parent of the just-deleted node, *ancestryStack[-3] to the grandparent, etc.
	//If we were to insert a black node there, we would have a valid RBT. Now we fix up the state of the tree.
	void eraseFixup(StackType &ancestryStack) requires (!HasParentPointer) {
		while (ancestryStack.size() > 1) {
			NodeType *current = *ancestryStack[-1];
			NodeType *parent = *ancestryStack[-2];
			Direction direction = getChildDirection(*parent, current);
			NodeType **sibling = &getChild(*parent, opposite(direction));
#ifdef PARANOID_RBT_VERIFICATION
		verifyAlmostRedBlackTree(ancestryStack);
#endif
			if (getColor(**sibling) == Color::Red) {
				setColor(**sibling, Color::Black);
				setColor(*parent, Color::Red);
				rotateAboutParent(ancestryStack, direction);
				continue;
			}

			NodeType *nearNephew = getChild(**sibling, direction);
			NodeType *farNephew = getChild(**sibling, opposite(direction));

			bool nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
			bool farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);

			if (!nearNephewIsRed && !farNephewIsRed) {
				setColor(**sibling, Color::Red);
				if (getColor(*parent) == Color::Red) {
					setColor(*parent, Color::Black);
					break;
				}
				ancestryStack.pop();
				continue;
			}

			if (!farNephewIsRed && nearNephewIsRed) {
				setColor(*nearNephew, Color::Black);
				setColor(**sibling, Color::Red);
				rotateSubtree(*sibling, opposite(direction));
				sibling = &getChild(*parent, opposite(direction));
				nearNephew = getChild(**sibling, direction);
				farNephew = getChild(**sibling, opposite(direction));

				nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
				farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);
			}

			if (farNephewIsRed) {
				setColor(**sibling, getColor(*parent));
				setColor(*parent, Color::Black);
				setColor(*farNephew, Color::Black);
				rotateAboutParent(ancestryStack, direction);
				break;
			}
		}
		if (this->root != nullptr) {
			setColor(*this->root, Color::Black);
		}
	}

	void eraseFixup(NodeType *parent, Direction direction) requires (HasParentPointer) {
		while (parent != nullptr) {
#ifdef PARANOID_RBT_VERIFICATION
		verifyAlmostRedBlackTree(parent, direction);
#endif
			NodeType *sibling = getChild(*parent, opposite(direction));

			if (getColor(*sibling) == Color::Red) {
				setColor(*sibling, Color::Black);
				setColor(*parent, Color::Red);
				rotateSubtree(parent, direction);
				continue;
			}

			NodeType *nearNephew = getChild(*sibling, direction);
			NodeType *farNephew = getChild(*sibling, opposite(direction));

			bool nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
			bool farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);

			if (!nearNephewIsRed && !farNephewIsRed) {
				setColor(*sibling, Color::Red);
				if (getColor(*parent) == Color::Red) {
					setColor(*parent, Color::Black);
					break;
				}
				NodeType *grandparent = RedBlackTreeInfoExtractor::parent(*parent);
				if (grandparent == nullptr) { break; }
				direction = getChildDirection(*grandparent, parent);
				parent = grandparent;
				continue;
			}

			if (!farNephewIsRed && nearNephewIsRed) {
				setColor(*nearNephew, Color::Black);
				setColor(*sibling, Color::Red);
				rotateSubtree(sibling, opposite(direction));
				sibling = getChild(*parent, opposite(direction));
				nearNephew = getChild(*sibling, direction);
				farNephew = getChild(*sibling, opposite(direction));
				nearNephewIsRed = (nearNephew != nullptr && getColor(*nearNephew) == Color::Red);
				farNephewIsRed = (farNephew != nullptr && getColor(*farNephew) == Color::Red);
			}

			if (farNephewIsRed) {
				setColor(*sibling, getColor(*parent));
				setColor(*parent, Color::Black);
				setColor(*farNephew, Color::Black);
				rotateSubtree(parent, direction);
				break;
			}
		}
		if (this->root != nullptr) {
			setColor(*this->root, Color::Black);
		}
	}

	//If the node has only one child, that child is red. We can replace toReplace with its child and recolor it
	//to black
	template<Direction direction>
	void eraseCaseSingleChild(NodeType *&node) {
		NodeType *child;
		if constexpr (direction == Direction::Left) {
			child = RedBlackTreeInfoExtractor::left(*node);
		} else {
			child = RedBlackTreeInfoExtractor::right(*node);
		}

		if constexpr (HasParentPointer) {
			RedBlackTreeInfoExtractor::parent(*child) = RedBlackTreeInfoExtractor::parent(*node);
		}
		node = child;
		setColor(*node, Color::Black);
	}

	//If the node we're erasing has 2 children, we have to replace it with its successor and possibly run a tree fixup
	//if the successor is black.
	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	Color eraseCaseTwoChildren(NodeType *&node, StackType &ancestryStack) requires (!HasParentPointer) {
		size_t ancestryStackFixupIndex = ancestryStack.size();
		NodeType **successorRef = &RedBlackTreeInfoExtractor::right(*node);
		ancestryStack.push(successorRef);
		while (hasLeftChild(*(*successorRef))) {
			successorRef = &RedBlackTreeInfoExtractor::left(*(*successorRef));
			ancestryStack.push(successorRef);
		}
		Color originalColor = getColor(*node);
		Color successorColor = getColor(*(*successorRef));
		NodeType *succ = *successorRef;

		RedBlackTreeInfoExtractor::left(*succ) = RedBlackTreeInfoExtractor::left(*node);
		//If the successor is not immediately to the right, also update its right child
		if (succ != RedBlackTreeInfoExtractor::right(*node)) {
			*successorRef = RedBlackTreeInfoExtractor::right(*succ);
			//If the successor is not immediately to the right, but it does have a right child, then that child is red.
			//Recolor it to black and we're done
			if (*successorRef != nullptr) {
				setColor(*(*successorRef), Color::Black);
				successorColor = Color::Red;
			}
			RedBlackTreeInfoExtractor::right(*succ) = RedBlackTreeInfoExtractor::right(*node);
		}
		//Otherwise the successor has no left child. Thus it possibly has a red leaf. If so, we can just recolor it to black
		//and we're done
		else {
			if (hasRightChild(*succ)) {
				setColor(*RedBlackTreeInfoExtractor::right(*succ), Color::Black);
				successorColor = Color::Red;
			}
		}
		node = succ;
		setColor(*succ, originalColor);
		ancestryStack[ancestryStackFixupIndex] = &RedBlackTreeInfoExtractor::right(*node);
		return successorColor;
	}

	//sets fixupStart to the
	Color eraseCaseTwoChildren(NodeType *&node, NodeType *&fixupStart, Direction &fixupDirection) requires (
		HasParentPointer) {
		NodeType *succ = RedBlackTreeInfoExtractor::right(*node);
		fixupDirection = Direction::Right;
		while (hasLeftChild(*succ)) {
			fixupDirection = Direction::Left;
			succ = RedBlackTreeInfoExtractor::left(*succ);
		}
		if (fixupDirection == Direction::Left) {
			fixupStart = RedBlackTreeInfoExtractor::parent(*succ);
		} else {
			fixupStart = succ;
		}

		Color originalColor = getColor(*node);
		Color successorColor = getColor(*succ);

		//Regardless of whether or not successor is node's right child, we need to update the left child of successor
		//to be node's old left child, and update that child's parent pointer if it's non-null
		NodeType *leftChild = RedBlackTreeInfoExtractor::left(*node);
		if (leftChild != nullptr) {
			RedBlackTreeInfoExtractor::parent(*leftChild) = succ;
		}
		RedBlackTreeInfoExtractor::left(*succ) = leftChild;

		//If successor is not immediately to the right of node, we also update the right child
		if (RedBlackTreeInfoExtractor::right(*node) != succ) {
			NodeType *oldRightChild = RedBlackTreeInfoExtractor::right(*succ);
			NodeType *parent = RedBlackTreeInfoExtractor::parent(*succ);
			if (oldRightChild != nullptr) {
				RedBlackTreeInfoExtractor::parent(*oldRightChild) = parent;
				setColor(*oldRightChild, Color::Black);
				successorColor = Color::Red;
			}
			RedBlackTreeInfoExtractor::left(*parent) = oldRightChild;

			NodeType *rightChild = RedBlackTreeInfoExtractor::right(*node);
			if (rightChild != nullptr) {
				RedBlackTreeInfoExtractor::parent(*rightChild) = succ;
			}
			RedBlackTreeInfoExtractor::right(*succ) = rightChild;
		}
		//Otherwise, we retain the reference to the existing right child. If that child is non-null, it must be red
		//since there was no prior left child. If we recolor to black, then no fixup needs to be done.
		else {
			NodeType *rightChild = RedBlackTreeInfoExtractor::right(*succ);
			if (rightChild != nullptr) {
				setColor(*rightChild, Color::Black);
				successorColor = Color::Red;
			}
		}
		RedBlackTreeInfoExtractor::parent(*succ) = RedBlackTreeInfoExtractor::parent(*node);
		node = succ;
		setColor(*succ, originalColor);
		return successorColor;
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	//Requires that ancestryStack.top() == &toRemove
	//The caller must populate the ancestry stack up to the point of toRemove
	void eraseImpl(NodeType *&toRemove, StackType &ancestryStack) requires (!HasParentPointer) {
		bool performFixup = false;
#ifdef PARANOID_RBT_VERIFICATION
	size_t preRemovalSize = getTreeSize();
#endif
		//Address simple cases first, like the degenerate case when toRemove == null
		if (toRemove == nullptr) { return; }
		//If the node is a leaf, we just remove it. We need to perform a fixup if the node is not the root but is black
		else if (!hasChild(*toRemove)) {
			if ((toRemove != this->root) && (getColor(*toRemove) == Color::Black)) {
				performFixup = true;
			}
			toRemove = nullptr;
		}
		//If the node is not a leaf, there are 3 simple cases, and one case that may necessitate running a fixup
		else if (hasChild(*toRemove)) {
			if (!hasLeftChild(*toRemove)) {
				eraseCaseSingleChild<Direction::Right>(toRemove);
			} else if (!hasRightChild(*toRemove)) {
				eraseCaseSingleChild<Direction::Left>(toRemove);
			} else {
				performFixup = (eraseCaseTwoChildren(toRemove, ancestryStack) == Color::Black);
			}
		}
		NodeType *parent = nullptr;
		if constexpr (AugmentedNode) {
			if (ancestryStack.size() > 1) {
				parent = *ancestryStack[-2];
			}
		}
		if (performFixup) {
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
		if constexpr (AugmentedNode) {
			if (parent != nullptr) {
				StackType augmentationAncestryStack;
				BSTParent::populateAncestryStack(RedBlackTreeInfoExtractor::data(*parent), augmentationAncestryStack);
				augmentationAncestryStack.push(&RedBlackTreeInfoExtractor::left(*parent));
				BSTParent::fixupAugmentationData(augmentationAncestryStack);
			}
			//assert(BinTreeParent::verifyAugmentationData(this -> root), "Augmentation data verification failed");
		}
	}

	void eraseImpl(NodeType *&toRemove) requires (HasParentPointer) {
		//assert(BinTreeParent::verifyParentPointers(), "eraseImpl start - parent pointers are invalid");
		bool performFixup = false;

		if (toRemove == nullptr) { return; }

		NodeType *fixupLocation = nullptr;
		Direction fixupDirection = Direction::Right;
		if (RedBlackTreeInfoExtractor::parent(*toRemove) != nullptr) {
			fixupLocation = RedBlackTreeInfoExtractor::parent(*toRemove);
		}
		if (!hasChild(*toRemove)) {
			if ((toRemove != this->root) && (getColor(*toRemove) == Color::Black)) {
				NodeType *parent = RedBlackTreeInfoExtractor::parent(*toRemove);
				fixupDirection = getChildDirection(*parent, toRemove);
				performFixup = true;
			}
			toRemove = nullptr;
		} else if (hasChild(*toRemove)) {
			if (!hasLeftChild(*toRemove)) {
				eraseCaseSingleChild<Direction::Right>(toRemove);
			} else if (!hasRightChild(*toRemove)) {
				eraseCaseSingleChild<Direction::Left>(toRemove);
			} else {
				performFixup = (eraseCaseTwoChildren(toRemove, fixupLocation, fixupDirection) == Color::Black);
			}
		}
		if (performFixup) {
			eraseFixup(fixupLocation, fixupDirection);
		}
		if constexpr (AugmentedNode) {
			if (fixupLocation != nullptr) {
				BSTParent::fixupAugmentationData(fixupLocation);
			}
		}
#ifdef PARANOID_RBT_VERIFICATION
		verifyRedBlackTree();
#endif
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	void insertFixup(StackType &ancestryStack) requires (!HasParentPointer) {
		while (ancestryStack.size() >= 3) {
			NodeType *node = *ancestryStack[-1]; // current node
			NodeType *parent = *ancestryStack[-2]; // parent
			NodeType *grandparent = *ancestryStack[-3]; // grandparent

			//If the parent is black, tree is already balanced
			if (getColor(*parent) == Color::Black) {
				break;
			}

			// Parent is red, so we need to fix
			Direction parentDirection = getChildDirection(*grandparent, parent);
			NodeType *uncle = getChild(*grandparent, opposite(parentDirection));

			if (uncle != nullptr && RedBlackTreeInfoExtractor::isRed(*uncle)) {
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

				if (nodeDirection != parentDirection) {
					// Case 2a: Triangle - first rotation
					rotateSubtree(*ancestryStack[-2], parentDirection);
				}

				// Case 2b: Line - rotate grandparent
				rotateSubtree(*ancestryStack[-3], opposite(parentDirection));

				// Recolor after rotations
				NodeType *newParent = *ancestryStack[-3]; // This is now the top after rotation
				setColor(*newParent, Color::Black);
				if (auto child = RedBlackTreeInfoExtractor::left(*newParent)) {
					setColor(*child, Color::Red);
				}
				if (auto child = RedBlackTreeInfoExtractor::right(*newParent)) {
					setColor(*child, Color::Red);
				}
				break;
			}
		}

		// Root must always be black
		if (this->root != nullptr) {
			setColor(*this->root, Color::Black);
		}
	}

	void insertFixup(NodeType &node) requires (HasParentPointer) {
		NodeType *current = &node;
		NodeType *parent, *grandparent;
		while ((parent = RedBlackTreeInfoExtractor::parent(*current)) && (
			       grandparent = RedBlackTreeInfoExtractor::parent(*parent))) {
			if (getColor(*parent) == Color::Black) {
				return;
			}

			Direction parentDirection = getChildDirection(*grandparent, parent);
			NodeType *uncle = getChild(*grandparent, opposite(parentDirection));

			if (uncle != nullptr && RedBlackTreeInfoExtractor::isRed(*uncle)) {
				setColor(*parent, Color::Black);
				setColor(*uncle, Color::Black);
				setColor(*grandparent, Color::Red);

				current = grandparent;
			} else {
				Direction nodeDirection = getChildDirection(*parent, current);
				NodeType *newParent = parent;
				if (nodeDirection != parentDirection) {
					newParent = current;
					rotateSubtree(parent, parentDirection);
				}

				rotateSubtree(grandparent, opposite(parentDirection));

				setColor(*newParent, Color::Black);
				if (auto child = RedBlackTreeInfoExtractor::left(*newParent)) {
					setColor(*child, Color::Red);
				}
				if (auto child = RedBlackTreeInfoExtractor::right(*newParent)) {
					setColor(*child, Color::Red);
				}
				break;
			}
		}
		if (this->root != nullptr) {
			setColor(*this->root, Color::Black);
		}
	}

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	bool insertImpl(NodeType *node) requires (!HasParentPointer) {
		if (node == nullptr) {
			return false;
		}
		RedBlackTreeInfoExtractor::left(*node) = nullptr;
		RedBlackTreeInfoExtractor::right(*node) = nullptr;
		if (this->root == nullptr) {
			this->root = node;
			setColor(*node, Color::Black);
			return true;
		}
		StackType ancestryStack;
		NodeType **current = &this->root;
		NodeDataWithReference nodeData = RedBlackTreeInfoExtractor::data(*node);
		do {
			ancestryStack.push(current);
			//if node is already present in the tree, bail
			NodeDataWithReference currentData = RedBlackTreeInfoExtractor::data(*(*current));
			if (nodeData == currentData) {
				return false;
			}
			//if node < current, go left
			if (comparator(nodeData, currentData)) {
				current = &RedBlackTreeInfoExtractor::left(*(*current));
			}
			//otherwise go right
			else {
				current = &RedBlackTreeInfoExtractor::right(*(*current));
			}
		} while (*current != nullptr);
		*current = node;
		//assert(RedBlackTreeInfoExtractor::left(*node) != node && RedBlackTreeInfoExtractor::right(*node) != node, "Inserted node was made its own parent");
		setColor(*node, Color::Red);
		ancestryStack.push(current);
		insertFixup(ancestryStack);
		return true;
	}

	bool insertImpl(NodeType *node) requires (HasParentPointer) {
		if (node == nullptr) {
			return false;
		}
		RedBlackTreeInfoExtractor::left(*node) = nullptr;
		RedBlackTreeInfoExtractor::right(*node) = nullptr;
		RedBlackTreeInfoExtractor::parent(*node) = nullptr;

		if (this->root == nullptr) {
			this->root = node;
			setColor(*node, Color::Black);
			return true;
		}

		NodeType **current = &(this->root);
		NodeDataWithReference nvalue = RedBlackTreeInfoExtractor::data(*node);
		NodeType *parent = nullptr;
		do {
			parent = *current;
			NodeDataWithReference cvalue = RedBlackTreeInfoExtractor::data(*(*current));

			if (nvalue == cvalue) {
				return false;
			}
			//if node < current, go left
			if (comparator(nvalue, cvalue)) {
				current = &RedBlackTreeInfoExtractor::left(*(*current));
			} else {
				current = &RedBlackTreeInfoExtractor::right(*(*current));
			}
		} while (*current != nullptr);
		*current = node;
		setColor(*node, Color::Red);
		RedBlackTreeInfoExtractor::parent(*node) = parent;
		insertFixup(*node);
		return true;
	}

	//This seems to be far too expensive to yield any sort of optimization for in-place updates
	//with the RBTs in LibAlloc
	bool verifyCorrectLocalOrder(NodeType& node) requires (HasParentPointer) {
		//Note that comparator(a, b) essentially returns a < b
		auto data = RedBlackTreeInfoExtractor::data(node);
		if (auto left = RedBlackTreeInfoExtractor::left(node)) {
			auto leftData = RedBlackTreeInfoExtractor::data(*left);
			//if node < left, bail
			if (comparator(data, leftData)) {
				return false;
			}
		}
		if (auto right = RedBlackTreeInfoExtractor::right(node)) {
			auto rightData = RedBlackTreeInfoExtractor::data(*right);
			//if right < node, bail
			if (comparator(rightData, data)) {
				return false;
			}
		}
		if (auto parent = RedBlackTreeInfoExtractor::parent(node)) {
			auto dir = getChildDirection(*parent, &node);
			auto parentData = RedBlackTreeInfoExtractor::data(*parent);
			if (dir == Direction::Left) {
				//If node is the left child of its parent, then we should have data < parentData
				if (comparator(parentData, data)) {
					return false;
				}
			}
			else {
				if (comparator(data, parentData)) {
					return false;
				}
			}
		}
		return true;
	}

public:
	using BSTParent::visitDepthFirstInOrder;
	using BSTParent::visitDepthFirstReverseOrder;
	using BSTParent::visitDepthFirstPostOrder;
	using BSTParent::find;
	using BSTParent::floor;
	using BSTParent::ceil;
	using BSTParent::mappedCeil;
	using BSTParent::mappedFloor;
	using BSTParent::min;
	using BSTParent::max;
	using BSTParent::successor;
	using BSTParent::predecessor;
	using BSTParent::getRoot;

	template<typename StackType>
		requires IsStack<NodeType **, StackType>
	bool insert(NodeType *node) requires (!HasParentPointer) {
#ifdef PARANOID_RBT_VERIFICATION
	size_t preInsertionSize = getTreeSize();
	verifyRedBlackTree();
#endif
		bool result = insertImpl<StackType>(node);
#ifdef PARANOID_RBT_VERIFICATION
	assert(getTreeSize() == preInsertionSize + (result ? 1 : 0), "Node count mismatch");
	verifyRedBlackTree();
#endif
		if constexpr (AugmentedNode) {
			StackType ancestryStack;
			BSTParent::populateAncestryStack(RedBlackTreeInfoExtractor::data(*node), ancestryStack);
			BSTParent::fixupAugmentationData(ancestryStack);
			//assert(BinTreeParent::verifyAugmentationData(this -> root), "Augmentation data verification failed");
		}
		return result;
	}

	bool insert(NodeType *node) requires (HasParentPointer) {
		bool result = insertImpl(node);
		if constexpr (AugmentedNode) {
			BSTParent::fixupAugmentationData(node);
		}
		return result;
	}

	template<typename StackType>
	NodeType *erase(NodeType *node) requires (!HasParentPointer) {
		return erase<StackType>(RedBlackTreeInfoExtractor::data(*node));
	}

	template<typename StackType>
	NodeType *erase(const NodeData &value) requires (!HasParentPointer) {
		StackType ancestryStack;
		NodeType **current = &this->root;
		while (*current != nullptr) {
			ancestryStack.push(current);
			NodeDataWithReference data = RedBlackTreeInfoExtractor::data(*(*current));
			if (value == data) {
				NodeType *toRemove = *current;
				//ancestryStack[0] = root, ancestryStack[-1] is node to be removed
				eraseImpl(*current, ancestryStack);
				return toRemove;
			}
			if (comparator(value, data)) {
				current = &RedBlackTreeInfoExtractor::left(*(*current));
			} else {
				current = &RedBlackTreeInfoExtractor::right(*(*current));
			}
		}
		return nullptr;
	}

	NodeType *erase(NodeType *node) requires (HasParentPointer) {
		if (node == nullptr) { return nullptr; }
		if (node == this->root) {
			eraseImpl(this->root);
			return node;
		}
		NodeType *parent = RedBlackTreeInfoExtractor::parent(*node);
		if (RedBlackTreeInfoExtractor::left(*parent) == node) {
			eraseImpl(RedBlackTreeInfoExtractor::left(*parent));
			return node;
		}
		eraseImpl(RedBlackTreeInfoExtractor::right(*parent));
		return node;
	}

	NodeType *erase(const NodeData &value) requires (HasParentPointer) {
		return erase(this->find(value));
	}


	template<typename StackType, typename Lambda>
	requires Invocable<Lambda, void, NodeType&>
	bool update(const NodeData &value, Lambda updateLambda) requires (!HasParentPointer) {
		NodeType *node = erase<StackType>(value);
		if (node != nullptr) {
			updateLambda(*node);
			insert<StackType>(node);
			return true;
		}
		return false;
	}

	template <typename Lambda>
	requires Invocable<Lambda, void, NodeType&>
	bool update(NodeType *node, Lambda updateLambda) requires (HasParentPointer) {
		if (node == nullptr) { return false; }
		auto color = getColor(*node);
		updateLambda(*node);
		setColor(*node, color);
		/*if (verifyCorrectLocalOrder(*node)) {
			BSTParent::propagateAugmentationRefresh(*node);
			return true;
		}*/
		erase(node);
		insert(node);
		return true;
	}

	template<typename StackType>
	bool recomputeAugmentationData(NodeType* node) requires (!HasParentPointer) {
		if (node == nullptr) { return false; }
		BSTParent::template propagateAugmentationRefresh<StackType>(node);
		return true;
	}

	bool recomputeAugmentationData(NodeType* node) requires (HasParentPointer) {
		if (node == nullptr) { return false; }
		BSTParent::propagateAugmentationRefresh(node);
		return true;
	}
};

// Value-owning tree node for standard (non-intrusive) trees
template<typename T, bool HasParent>
struct TreeNode;

template<typename T>
struct TreeNode<T, false> {
	T data;
	TreeNode *left;
	TreeNode *right;

	TreeNode(const T &value) : data(value), left(nullptr), right(nullptr) {
	}

	TreeNode(T &&value) : data(move(value)), left(nullptr), right(nullptr) {
	}
};

template<typename T>
struct TreeNode<T, true> {
	T data;
	TreeNode *left;
	TreeNode *right;
	TreeNode *parent;

	TreeNode(const T &value) : data(value), left(nullptr), right(nullptr), parent(nullptr) {
	}

	TreeNode(T &&value) : data(move(value)), left(nullptr), right(nullptr), parent(nullptr) {
	}
};

// Extractor for TreeNode
template<typename T, bool HasParent>
struct TreeNodeExtractor;

template<typename T>
struct TreeNodeExtractor<T, false> {
	static TreeNode<T, false> *&left(TreeNode<T, false> &node) { return node.left; }
	static TreeNode<T, false> *&right(TreeNode<T, false> &node) { return node.right; }
	static TreeNode<T, false> *const&left(const TreeNode<T, false> &node) { return node.left; }
	static TreeNode<T, false> *const&right(const TreeNode<T, false> &node) { return node.right; }
	static T &data(TreeNode<T, false> &node) { return node.data; }
	static const T &data(const TreeNode<T, false> &node) { return node.data; }
};

template<typename T>
struct TreeNodeExtractor<T, true> {
	static TreeNode<T, true> *&left(TreeNode<T, true> &node) { return node.left; }
	static TreeNode<T, true> *&right(TreeNode<T, true> &node) { return node.right; }
	static TreeNode<T, true> *&parent(TreeNode<T, true> &node) { return node.parent; }
	static TreeNode<T, true> *const&left(const TreeNode<T, true> &node) { return node.left; }
	static TreeNode<T, true> *const&right(const TreeNode<T, true> &node) { return node.right; }
	static TreeNode<T, true> *const&parent(const TreeNode<T, true> &node) { return node.parent; }
	static T &data(TreeNode<T, true> &node) { return node.data; }
	static const T &data(const TreeNode<T, true> &node) { return node.data; }
};

// Value-owning Binary Tree
template<typename T, bool HasParent>
class BinaryTreeBase : private IntrusiveBinaryTree<TreeNode<T, HasParent>, TreeNodeExtractor<T, HasParent> > {
	using Node = TreeNode<T, HasParent>;
	using Parent = IntrusiveBinaryTree<Node, TreeNodeExtractor<T, HasParent> >;

	void deleteSubtree(Node *node) {
		if (node != nullptr) {
			deleteSubtree(node->left);
			deleteSubtree(node->right);
			delete node;
		}
	}

public:
	BinaryTreeBase() = default;

	// Constructor with root value
	explicit BinaryTreeBase(const T &rootValue) {
		this->root = new Node(rootValue);
	}

	explicit BinaryTreeBase(T &&rootValue) {
		this->root = new Node(move(rootValue));
	}

	~BinaryTreeBase() {
		deleteSubtree(this->root);
	}

	// Delete copy constructor and assignment - trees are move-only for simplicity
	BinaryTreeBase(const BinaryTreeBase &) = delete;

	BinaryTreeBase &operator=(const BinaryTreeBase &) = delete;

	// Move constructor and assignment
	BinaryTreeBase(BinaryTreeBase &&other) noexcept : Parent(other.root) {
		other.root = nullptr;
	}

	BinaryTreeBase &operator=(BinaryTreeBase &&other) noexcept {
		if (this != &other) {
			deleteSubtree(this->root);
			this->root = other.root;
			other.root = nullptr;
		}
		return *this;
	}

	// Set root (replaces existing tree)
	void setRoot(const T &value) {
		deleteSubtree(this->root);
		this->root = new Node(value);
	}

	void setRoot(T &&value) {
		deleteSubtree(this->root);
		this->root = new Node(move(value));
	}

	// Manual tree building methods
	void setLeftChild(Node *parent, const T &value) {
		if (parent != nullptr && parent->left == nullptr) {
			parent->left = new Node(value);
		}
	}

	void setRightChild(Node *parent, const T &value) {
		if (parent != nullptr && parent->right == nullptr) {
			parent->right = new Node(value);
		}
	}

	Node *getRoot() { return this->root; }
	const Node *getRoot() const { return this->root; }

	// Expose visitor methods
	using Parent::visitDepthFirstInOrder;
	using Parent::visitDepthFirstReverseOrder;
	using Parent::visitDepthFirstPostOrder;

	bool empty() const { return this->root == nullptr; }
};

template<typename T>
using BinaryTreeWithoutParents = BinaryTreeBase<T, false>;

template<typename T>
using BinaryTree = BinaryTreeBase<T, true>;

// Value-owning Binary Search Tree
template<typename T, bool HasParent, typename Comparator>
class BinarySearchTreeBase : private IntrusiveBinarySearchTree<TreeNode<T, HasParent>, TreeNodeExtractor<T, HasParent>,
			Comparator> {
	using Node = TreeNode<T, HasParent>;
	using Parent = IntrusiveBinarySearchTree<Node, TreeNodeExtractor<T, HasParent>, Comparator>;

	void deleteSubtree(Node *node) {
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
	BinarySearchTreeBase(const BinarySearchTreeBase &) = delete;

	BinarySearchTreeBase &operator=(const BinarySearchTreeBase &) = delete;

	// Move constructor and assignment
	BinarySearchTreeBase(BinarySearchTreeBase &&other) noexcept : Parent() {
		this->root = other.root;
		this->comparator = move(other.comparator);
		other.root = nullptr;
	}

	BinarySearchTreeBase &operator=(BinarySearchTreeBase &&other) noexcept {
		if (this != &other) {
			deleteSubtree(this->root);
			this->root = other.root;
			this->comparator = move(other.comparator);
			other.root = nullptr;
		}
		return *this;
	}

	// Insert operations
	void insert(const T &value) {
		Node *node = new Node(value);
		Parent::insert(node);
	}

	void insert(T &&value) {
		Node *node = new Node(move(value));
		Parent::insert(node);
	}

	// Find operation
	bool contains(const T &value) const {
		return Parent::find(value) != nullptr;
	}

	// Erase operation
	bool erase(const T &value) {
		Node *node = Parent::erase(value);
		if (node != nullptr) {
			delete node;
			return true;
		}
		return false;
	}

	// Floor operation - largest element <= value
	bool floor(const T &value, T &result) const {
		const Node *node = Parent::floor(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Ceil operation - smallest element >= value
	bool ceil(const T &value, T &result) const {
		const Node *node = Parent::ceil(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Successor operation - next larger element
	bool successor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *succ = Parent::successor(node);
			if (succ != nullptr) {
				result = succ->data;
				return true;
			}
		}
		return false;
	}

	// Predecessor operation - next smaller element
	bool predecessor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *pred = Parent::predecessor(node);
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

template<typename T, typename Comparator = DefaultComparator<T> >
using BinarySearchTreeWithoutParents = BinarySearchTreeBase<T, false, Comparator>;

template<typename T, typename Comparator = DefaultComparator<T> >
using BinarySearchTree = BinarySearchTreeBase<T, true, Comparator>;

// Red-black tree node for value-owning trees
template<typename T, bool HasParent>
struct PlainRedBlackTreeNode;

template<typename T>
struct PlainRedBlackTreeNode<T, false> {
	T data;
	PlainRedBlackTreeNode *left;
	PlainRedBlackTreeNode *right;
	bool isRed;

	PlainRedBlackTreeNode(const T &value) : data(value), left(nullptr), right(nullptr), isRed(true) {
	}

	PlainRedBlackTreeNode(T &&value) : data(move(value)), left(nullptr), right(nullptr), isRed(true) {
	}
};

template<typename T>
struct PlainRedBlackTreeNode<T, true> {
	T data;
	PlainRedBlackTreeNode *left;
	PlainRedBlackTreeNode *right;
	PlainRedBlackTreeNode *parent;
	bool isRed;

	PlainRedBlackTreeNode(const T &value) : data(value), left(nullptr), right(nullptr), parent(nullptr), isRed(true) {
	}

	PlainRedBlackTreeNode(T &&value) : data(move(value)), left(nullptr), right(nullptr), parent(nullptr), isRed(true) {
	}
};

template<typename T, typename S, bool HasParent>
struct AugmentedRedBlackTreeNode;

template<typename T, typename S>
struct AugmentedRedBlackTreeNode<T, S, false> {
	T data;
	S augmentationData;
	AugmentedRedBlackTreeNode *left;
	AugmentedRedBlackTreeNode *right;
	bool isRed;

	AugmentedRedBlackTreeNode(const T &value) : data(value), left(nullptr), right(nullptr), isRed(true),
	                                            augmentationData(S()) {
	}

	AugmentedRedBlackTreeNode(T &&value) : data(move(value)), left(nullptr), right(nullptr), isRed(true),
	                                       augmentationData(S()) {
	}
};

template<typename T, typename S>
struct AugmentedRedBlackTreeNode<T, S, true> {
	T data;
	S augmentationData;
	AugmentedRedBlackTreeNode *left;
	AugmentedRedBlackTreeNode *right;
	AugmentedRedBlackTreeNode *parent;
	bool isRed;

	AugmentedRedBlackTreeNode(const T &value) : data(value), augmentationData(S()), left(nullptr), right(nullptr),
	                                            parent(nullptr), isRed(true) {
	}

	AugmentedRedBlackTreeNode(T &&value) : data(move(value)), augmentationData(S()), left(nullptr), right(nullptr),
	                                       parent(nullptr), isRed(true) {
	}
};

// Extractor for RedBlackTreeNode
template<typename T, bool HasParent>
struct PlainRedBlackTreeNodeExtractor;

template<typename T>
struct PlainRedBlackTreeNodeExtractor<T, false> {
	static PlainRedBlackTreeNode<T, false> *&left(PlainRedBlackTreeNode<T, false> &node) { return node.left; }
	static PlainRedBlackTreeNode<T, false> *&right(PlainRedBlackTreeNode<T, false> &node) { return node.right; }

	static PlainRedBlackTreeNode<T, false> *const&left(const PlainRedBlackTreeNode<T, false> &node) {
		return node.left;
	}

	static PlainRedBlackTreeNode<T, false> *const&right(const PlainRedBlackTreeNode<T, false> &node) {
		return node.right;
	}

	static T &data(PlainRedBlackTreeNode<T, false> &node) { return node.data; }
	static const T &data(const PlainRedBlackTreeNode<T, false> &node) { return node.data; }
	static bool isRed(const PlainRedBlackTreeNode<T, false> &node) { return node.isRed; }
	static void setRed(PlainRedBlackTreeNode<T, false> &node, bool red) { node.isRed = red; }
};

template<typename T>
struct PlainRedBlackTreeNodeExtractor<T, true> {
	static PlainRedBlackTreeNode<T, true> *&left(PlainRedBlackTreeNode<T, true> &node) { return node.left; }
	static PlainRedBlackTreeNode<T, true> *&right(PlainRedBlackTreeNode<T, true> &node) { return node.right; }
	static PlainRedBlackTreeNode<T, true> *&parent(PlainRedBlackTreeNode<T, true> &node) { return node.parent; }
	static PlainRedBlackTreeNode<T, true> *const&left(const PlainRedBlackTreeNode<T, true> &node) { return node.left; }

	static PlainRedBlackTreeNode<T, true> *const&right(const PlainRedBlackTreeNode<T, true> &node) {
		return node.right;
	}

	static PlainRedBlackTreeNode<T, true> *const&parent(const PlainRedBlackTreeNode<T, true> &node) {
		return node.parent;
	}

	static T &data(PlainRedBlackTreeNode<T, true> &node) { return node.data; }
	static const T &data(const PlainRedBlackTreeNode<T, true> &node) { return node.data; }
	static bool isRed(const PlainRedBlackTreeNode<T, true> &node) { return node.isRed; }
	static void setRed(PlainRedBlackTreeNode<T, true> &node, bool red) { node.isRed = red; }
};

// Extractor for RedBlackTreeNode
template<typename T, typename S, typename AugmentationAccumulator, bool HasParent>
struct AugmentedRedBlackTreeNodeExtractor;

template<typename T, typename S, typename AugmentationAccumulator>
struct AugmentedRedBlackTreeNodeExtractor<T, S, AugmentationAccumulator, false> {
	static AugmentedRedBlackTreeNode<T, S, false> *&left(AugmentedRedBlackTreeNode<T, S, false> &node) {
		return node.left;
	}

	static AugmentedRedBlackTreeNode<T, S, false> *&right(AugmentedRedBlackTreeNode<T, S, false> &node) {
		return node.right;
	}

	static AugmentedRedBlackTreeNode<T, S, false> *const&left(const AugmentedRedBlackTreeNode<T, S, false> &node) {
		return node.left;
	}

	static AugmentedRedBlackTreeNode<T, S, false> *const&right(const AugmentedRedBlackTreeNode<T, S, false> &node) {
		return node.right;
	}

	static T &data(AugmentedRedBlackTreeNode<T, S, false> &node) { return node.data; }
	static const T &data(const AugmentedRedBlackTreeNode<T, S, false> &node) { return node.data; }
	static bool isRed(const AugmentedRedBlackTreeNode<T, S, false> &node) { return node.isRed; }
	static void setRed(AugmentedRedBlackTreeNode<T, S, false> &node, bool red) { node.isRed = red; }
	static S &augmentedData(AugmentedRedBlackTreeNode<T, S, false> &node) { return node.augmentationData; }
	static const S &augmentedData(const AugmentedRedBlackTreeNode<T, S, false> &node) { return node.augmentationData; }

	static S recomputeAugmentedData(const AugmentedRedBlackTreeNode<T, S, false> &node,
	                                const AugmentedRedBlackTreeNode<T, S, false> *left,
	                                const AugmentedRedBlackTreeNode<T, S, false> *right) {
		AugmentationAccumulator accumulator;
		const T &nodeData = data(node);
		const S *leftData = (left != nullptr) ? &augmentedData(*left) : nullptr;
		const S *rightData = (right != nullptr) ? &augmentedData(*right) : nullptr;
		return accumulator(nodeData, leftData, rightData);
	}
};

template<typename T, typename S, typename AugmentationAccumulator>
struct AugmentedRedBlackTreeNodeExtractor<T, S, AugmentationAccumulator, true> {
	static AugmentedRedBlackTreeNode<T, S, true> *&left(AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.left;
	}

	static AugmentedRedBlackTreeNode<T, S, true> *&right(AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.right;
	}

	static AugmentedRedBlackTreeNode<T, S, true> *&parent(AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.parent;
	}

	static AugmentedRedBlackTreeNode<T, S, true> *const&left(const AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.left;
	}

	static AugmentedRedBlackTreeNode<T, S, true> *const&right(const AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.right;
	}

	static AugmentedRedBlackTreeNode<T, S, true> *const&parent(const AugmentedRedBlackTreeNode<T, S, true> &node) {
		return node.parent;
	}

	static T &data(AugmentedRedBlackTreeNode<T, S, true> &node) { return node.data; }
	static const T &data(const AugmentedRedBlackTreeNode<T, S, true> &node) { return node.data; }
	static bool isRed(const AugmentedRedBlackTreeNode<T, S, true> &node) { return node.isRed; }
	static void setRed(AugmentedRedBlackTreeNode<T, S, true> &node, bool red) { node.isRed = red; }
	static S &augmentedData(AugmentedRedBlackTreeNode<T, S, true> &node) { return node.augmentationData; }
	static const S &augmentedData(const AugmentedRedBlackTreeNode<T, S, true> &node) { return node.augmentationData; }

	static S recomputeAugmentedData(const AugmentedRedBlackTreeNode<T, S, true> &node,
	                                const AugmentedRedBlackTreeNode<T, S, true> *left,
	                                const AugmentedRedBlackTreeNode<T, S, true> *right) {
		AugmentationAccumulator accumulator;
		const T &nodeData = data(node);
		const S *leftData = (left != nullptr) ? &augmentedData(*left) : nullptr;
		const S *rightData = (right != nullptr) ? &augmentedData(*right) : nullptr;
		return accumulator(nodeData, leftData, rightData);
	}
};

template<typename T, typename AI>
concept ValidAugmentationInfo = requires(const T t, const typename AI::Data *left, const typename AI::Data *right)
{
	{ typename AI::Accumulator{}(t, left, right) } -> IsSame<typename AI::Data>;
};

template<typename T, typename AugmentationInfo, bool HasParent>
struct RedBlackTreeNodeHelper;

template<typename T, bool HasParent>
struct RedBlackTreeNodeHelper<T, NoAugmentation, HasParent> {
	using type = PlainRedBlackTreeNode<T, HasParent>;
};

template<typename T, typename AugmentationInfo, bool HasParent>
	requires ValidAugmentationInfo<T, AugmentationInfo>
struct RedBlackTreeNodeHelper<T, AugmentationInfo, HasParent> {
	using type = AugmentedRedBlackTreeNode<T, typename AugmentationInfo::Data, HasParent>;
};

template<typename T, typename AugmentationInfo, bool HasParent>
using RedBlackTreeNode = RedBlackTreeNodeHelper<T, AugmentationInfo, HasParent>::type;

template<typename T, typename AugmentationInfo, bool HasParent>
struct RedBlackTreeInfoExtractorHelper;

template<typename T, bool HasParent>
struct RedBlackTreeInfoExtractorHelper<T, NoAugmentation, HasParent> {
	using type = PlainRedBlackTreeNodeExtractor<T, HasParent>;
};

template<typename T, typename AugmentationInfo, bool HasParent>
	requires ValidAugmentationInfo<T, AugmentationInfo>
struct RedBlackTreeInfoExtractorHelper<T, AugmentationInfo, HasParent> {
	using type = AugmentedRedBlackTreeNodeExtractor<T, typename AugmentationInfo::Data, typename
		AugmentationInfo::Accumulator, HasParent>;
};

template<typename T, typename AugmentationInfo, bool HasParent>
using RedBlackTreeInfoExtractor = RedBlackTreeInfoExtractorHelper<T, AugmentationInfo, HasParent>::type;

// Value-owning Red-Black Tree
template<typename T, typename AugmentationInfo = NoAugmentation, typename Comparator = DefaultComparator<T>, typename
	StackType = StaticStack<RedBlackTreeNode<T, AugmentationInfo, false> **, 64> >
class GeneralParentlessRedBlackTree : private IntrusiveRedBlackTree<RedBlackTreeNode<T, AugmentationInfo, false>,
			RedBlackTreeInfoExtractor<T, AugmentationInfo, false>, Comparator> {
	using Node = RedBlackTreeNode<T, AugmentationInfo, false>;
	using Parent = IntrusiveRedBlackTree<Node, RedBlackTreeInfoExtractor<T, AugmentationInfo, false>, Comparator>;

	void deleteSubtree(Node *node) {
		if (node != nullptr) {
			deleteSubtree(node->left);
			deleteSubtree(node->right);
			delete node;
		}
	}

public:
	GeneralParentlessRedBlackTree() = default;

	explicit GeneralParentlessRedBlackTree(Comparator comp) : Parent() {
		this->comparator = comp;
	}

	~GeneralParentlessRedBlackTree() {
		deleteSubtree(this->root);
	}

	// Delete copy constructor and assignment
	GeneralParentlessRedBlackTree(const GeneralParentlessRedBlackTree &) = delete;

	GeneralParentlessRedBlackTree &operator=(const GeneralParentlessRedBlackTree &) = delete;

	// Move constructor and assignment
	GeneralParentlessRedBlackTree(GeneralParentlessRedBlackTree &&other) noexcept : Parent() {
		this->root = other.root;
		this->comparator = move(other.comparator);
		other.root = nullptr;
	}

	GeneralParentlessRedBlackTree &operator=(GeneralParentlessRedBlackTree &&other) noexcept {
		if (this != &other) {
			deleteSubtree(this->root);
			this->root = other.root;
			this->comparator = move(other.comparator);
			other.root = nullptr;
		}
		return *this;
	}

	// Insert operations
	void insert(const T &value) {
		Node *node = new Node(value);
		if (!Parent::template insert<StackType>(node)) {
			delete node;
		}
	}

	void insert(T &&value) {
		Node *node = new Node(move(value));
		if (!Parent::template insert<StackType>(node)) {
			delete node;
		}
	}

	// Find operation
	bool contains(const T &value) const {
		return Parent::find(value) != nullptr;
	}

	// Erase operation
	bool erase(const T &value) {
		auto erasedNode = Parent::template erase<StackType>(value);
		if (erasedNode != nullptr) {
			delete erasedNode;
			return true;
		}
		return false;
	}

	// Floor operation - largest element <= value
	bool floor(const T &value, T &result) const {
		const Node *node = Parent::floor(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Ceil operation - smallest element >= value
	bool ceil(const T &value, T &result) const {
		const Node *node = Parent::ceil(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Successor operation - next larger element
	bool successor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *succ = Parent::successor(node);
			if (succ != nullptr) {
				result = succ->data;
				return true;
			}
		}
		return false;
	}

	// Predecessor operation - next smaller element
	bool predecessor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *pred = Parent::predecessor(node);
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

	const Node *getRoot() const { return this->root; }
};

// Value-owning Red-Black Tree with parents
template<typename T, typename AugmentationInfo = NoAugmentation, typename Comparator = DefaultComparator<T> >
class GeneralRedBlackTree : private IntrusiveRedBlackTree<RedBlackTreeNode<T, AugmentationInfo, true>,
			RedBlackTreeInfoExtractor<T, AugmentationInfo, true>, Comparator> {
	using Node = RedBlackTreeNode<T, AugmentationInfo, true>;
	using Parent = IntrusiveRedBlackTree<Node, RedBlackTreeInfoExtractor<T, AugmentationInfo, true>, Comparator>;

	void deleteSubtree(Node *node) {
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
	GeneralRedBlackTree(const GeneralRedBlackTree &) = delete;

	GeneralRedBlackTree &operator=(const GeneralRedBlackTree &) = delete;

	// Move constructor and assignment
	GeneralRedBlackTree(GeneralRedBlackTree &&other) noexcept : Parent() {
		this->root = other.root;
		this->comparator = move(other.comparator);
		other.root = nullptr;
	}

	GeneralRedBlackTree &operator=(GeneralRedBlackTree &&other) noexcept {
		if (this != &other) {
			deleteSubtree(this->root);
			this->root = other.root;
			this->comparator = move(other.comparator);
			other.root = nullptr;
		}
		return *this;
	}

	// Insert operations
	void insert(const T &value) {
		Node *node = new Node(value);
		if (!Parent::insert(node)) {
			delete node;
		}
	}

	void insert(T &&value) {
		Node *node = new Node(move(value));
		if (!Parent::insert(node)) {
			delete node;
		}
	}

	// Find operation
	bool contains(const T &value) const {
		return Parent::find(value) != nullptr;
	}

	// Erase operation
	bool erase(const T &value) {
		auto erasedNode = Parent::erase(value);
		if (erasedNode != nullptr) {
			delete erasedNode;
			return true;
		}
		return false;
	}

	// Floor operation - largest element <= value
	bool floor(const T &value, T &result) const {
		const Node *node = Parent::floor(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Ceil operation - smallest element >= value
	bool ceil(const T &value, T &result) const {
		const Node *node = Parent::ceil(value);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Successor operation - next larger element
	bool successor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *succ = Parent::successor(node);
			if (succ != nullptr) {
				result = succ->data;
				return true;
			}
		}
		return false;
	}

	// Predecessor operation - next smaller element
	bool predecessor(const T &value, T &result) const {
		const Node *node = Parent::find(value);
		if (node != nullptr) {
			const Node *pred = Parent::predecessor(node);
			if (pred != nullptr) {
				result = pred->data;
				return true;
			}
		}
		return false;
	}

	template<typename V, typename F, typename TComparator = DefaultComparator<V>>
		requires Invocable<F, V, T> && Invocable<TComparator, bool, V, V>
	bool mappedCeil(const V val, T& result, F transform) const {
		auto* node = Parent::mappedCeil(val, transform);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	template<typename V, typename F, typename TComparator = DefaultComparator<V>>
		requires Invocable<F, V, T> && Invocable<TComparator, bool, V, V>
	bool mappedFloor(const V val, T& result, F transform) const {
		auto* node = Parent::mappedFloor(val, transform);
		if (node != nullptr) {
			result = node->data;
			return true;
		}
		return false;
	}

	// Expose visitor methods
	using Parent::visitDepthFirstInOrder;
	using Parent::visitDepthFirstReverseOrder;
	using Parent::visitDepthFirstPostOrder;

	bool empty() const { return this->root == nullptr; }

	const Node *getRoot() const { return this->root; }
};

// Helper struct to package augmentation info for AugmentedRedBlackTree
template<typename AugData, typename AugAccumulator>
struct AugmentationPackage {
	using Data = AugData;
	using Accumulator = AugAccumulator;
};

// Convenience alias for augmented red-black trees

template<typename T, typename Comparator = DefaultComparator<T>, typename StackType = StaticStack<RedBlackTreeNode<T,
	NoAugmentation, false> **, 64> >
using ParentlessRedBlackTree = GeneralParentlessRedBlackTree<T, NoAugmentation, Comparator, StackType>;

template<typename T, typename Comparator = DefaultComparator<T> >
using RedBlackTree = GeneralRedBlackTree<T, NoAugmentation, Comparator>;

template<typename T, typename AugData, typename AugAccumulator, typename Comparator = DefaultComparator<T>, typename
	StackType = StaticStack<RedBlackTreeNode<T, AugmentationPackage<AugData, AugAccumulator>, false> **, 64> >
using ParentlessAugmentedRedBlackTree = GeneralParentlessRedBlackTree<T, AugmentationPackage<AugData, AugAccumulator>,
	Comparator, StackType>;

template<typename T, typename AugData, typename AugAccumulator, typename Comparator = DefaultComparator<T> >
using AugmentedRedBlackTree = GeneralRedBlackTree<T, AugmentationPackage<AugData, AugAccumulator>, Comparator>;


#endif //TREES_H
