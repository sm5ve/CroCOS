//
// Routing graph type system: vertex labels, trigger-type metadata, routing constraints,
// the RoutingGraphBuilder, and the ContextDependentRoutableDomain abstraction.
//
// Include this header (or interrupts.h) in code that participates in building or
// querying the interrupt routing graph.
//

#ifndef CROCOS_INTERRUPT_ROUTING_GRAPH_H
#define CROCOS_INTERRUPT_ROUTING_GRAPH_H

#include <interrupts/InterruptDomains.h>

namespace kernel::interrupts {
   namespace managed {
      enum class NodeType { Device, Input };
      struct RoutingConstraint;

      // A vertex label in the routing graph. The meaning of index() is dual:
      //   - NodeType::Input  (domain implements InterruptReceiver): index() is the receiver index.
      //   - NodeType::Device (domain is a pure emitter):            index() is the emitter index.
      // Always check getType() before interpreting index() in context-sensitive code.
      class RoutingNodeLabel{
      private:
         friend struct RoutingConstraint;
         SharedPtr<platform::InterruptDomain> dom;
         size_t ind;
      public:


         SharedPtr<platform::InterruptDomain> domain() const{return dom;}
         size_t index() const{return ind;};

         RoutingNodeLabel(SharedPtr<platform::InterruptDomain>&& d, size_t i) : dom(move(d)), ind(i) {}
         RoutingNodeLabel(const SharedPtr<platform::InterruptDomain>& d, size_t i) : dom(d), ind(i) {}
         bool operator==(const RoutingNodeLabel& other) const = default;
         [[nodiscard]] size_t hash() const;

         [[nodiscard]] NodeType getType() const {
            if (domain() -> instanceof(TypeID_v<platform::InterruptReceiver>))
               return NodeType::Input;
            return NodeType::Device;
         };

#ifdef CROCOS_TESTING
         // Accessors for testing only
         const SharedPtr<platform::InterruptDomain>& getDomain() const { return dom; }
         size_t getIndex() const { return index(); }
#endif
      };
   }
}

template<>
struct DefaultHasher<kernel::interrupts::managed::RoutingNodeLabel> {
   size_t operator()(const kernel::interrupts::managed::RoutingNodeLabel& label) const;
};

namespace kernel::interrupts {
   namespace managed {
      enum class RoutingNodeTriggerType : size_t {
         TRIGGER_LEVEL = 0,
         TRIGGER_EDGE = 1,
         TRIGGER_UNDETERMINED = 2
      };

      struct RoutingNodeMetadata {
         RoutingNodeTriggerType triggerType = RoutingNodeTriggerType::TRIGGER_UNDETERMINED;
         Optional<SharedPtr<platform::InterruptDomain>> owner;
      };

      using RoutingVertexConfig = GraphProperties::ColoredLabeledVertex<RoutingNodeMetadata, RoutingNodeLabel>;
      using RoutingEdgeConfig = GraphProperties::PlainEdge;
      using RoutingGraphStructure = GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph, GraphPredicates::DirectedAcyclic>;
      using RoutingGraph = Graph<RoutingVertexConfig, RoutingEdgeConfig, RoutingGraphStructure>;

      template <bool Forward>
      class PotentialEdgeIterator {
      private:
         using BackwardIteratorRange = decltype(declval<topology::TopologyGraph>().incomingEdges(
            declval<typename topology::TopologyGraph::Vertex>()));
         using ForwardIteratorRange = decltype(declval<topology::TopologyGraph>().outgoingEdges(
            declval<typename topology::TopologyGraph::Vertex>()));
         using IteratorRange = conditional_t<Forward, ForwardIteratorRange, BackwardIteratorRange>;
         using Iterator = typename IteratorRange::Iterator;

         Iterator currentConnector;
         Iterator endConnector;
         size_t currentIndex;
         const SharedPtr<platform::InterruptDomain> fixedDomain;
         const size_t fixedIndex;
         const GraphBuilderBase<RoutingGraph>* graph;
         const bool checkTriggerType;
         friend struct RoutingConstraint;
         struct None{};
         PotentialEdgeIterator(const SharedPtr<platform::InterruptDomain>& domain, Iterator& itr,
            Iterator& end, size_t index, size_t findex, const GraphBuilderBase<RoutingGraph>* g, bool checkTriggerType);
         void advanceIntermediateState();
         [[nodiscard]] bool isValidIntermediateState();
         void advanceToValidState();
      public:
         bool operator!=(const PotentialEdgeIterator& other) const {
            return (currentConnector != other.currentConnector) || (currentIndex != other.currentIndex);
         }

         PotentialEdgeIterator& operator++();
         BuilderVertexHandle<RoutingGraph> operator*() const;
      };

      struct RoutingConstraint {
         using Builder = GraphBuilderBase<RoutingGraph>;
         using VertexHandle = BuilderVertexHandle<RoutingGraph>;
         static bool isEdgeAllowedImpl(const Builder& graph, VertexHandle source, VertexHandle target, bool checkTriggerType);
         static IteratorRange<PotentialEdgeIterator<true>> validEdgesFromImpl(const Builder& graph, VertexHandle source, bool checkTriggerType);
         static IteratorRange<PotentialEdgeIterator<false>> validEdgesToImpl(const Builder& graph, VertexHandle target, bool checkTriggerType);
         static bool isEdgeAllowed(const Builder& graph, VertexHandle source, VertexHandle target);
         static IteratorRange<PotentialEdgeIterator<true>> validEdgesFrom(const Builder& graph, VertexHandle source);
         static IteratorRange<PotentialEdgeIterator<false>> validEdgesTo(const Builder& graph, VertexHandle target);
      };

      class RoutingGraphBuilder : RestrictedGraphBuilder<RoutingGraph, RoutingConstraint> {
         using Base = RestrictedGraphBuilder<RoutingGraph, RoutingConstraint>;
         void setConnectedComponentTriggerType(Base::VertexHandle, RoutingNodeTriggerType type);
         friend class FreelyRoutableDomainGreedyRouter;

         template <bool Forward>
         using FilteredPotentialEdgeIterator = SimpleGraphFilteredIteratorRange<IteratorRange<PotentialEdgeIterator<Forward>>, Forward>;

         [[nodiscard]] bool isEdgeAllowedIgnoringTriggerType(Base::VertexHandle source, Base::VertexHandle target);
         [[nodiscard]] FilteredPotentialEdgeIterator<true> validEdgesFromIgnoringTriggerType(VertexHandle source);
         [[nodiscard]] FilteredPotentialEdgeIterator<false> validEdgesToIgnoringTriggerType(VertexHandle target);
      public:
         static RoutingGraphBuilder& fromGenericBuilder(RoutingConstraint::Builder&);
         static const RoutingGraphBuilder& fromGenericBuilder(const RoutingConstraint::Builder&);
         template<typename VertexContainer>
         explicit RoutingGraphBuilder(const VertexContainer& vertices);
         Optional<RoutingGraph> build();

         using Base::EdgeHandle;
         using Base::VertexHandle;
         using Base::getCurrentVertexCount;
         using Base::getCurrentEdgeCount;
         using Base::hasEdge;
         using Base::getOutgoingEdgeCount;
         using Base::getIncomingEdgeCount;
         using Base::getVertexLabel;
         using Base::getEdgeLabel;
         using Base::getEdgeWeight;
         using Base::getEdgeSource;
         using Base::getEdgeTarget;
         using Base::getVertices;
         using Base::getVertex;
         using Base::getVertexByLabel;
         using Base::getEdgeByLabel;
         using Base::currentVertices;
         using Base::currentEdges;
         using Base::unpopulatedVertices;
         using Base::unpopulatedEdges;
         using Base::reset;
         using Base::isEdgeFullyPopulated;
         using Base::getValidEdgesTo;
         using Base::getValidEdgesFrom;
         using Base::canAddEdge;
         using Base::clearEdgeLabel;
         using Base::setEdgeWeight;
         using Base::setEdgeLabel;
         using Base::getConstraint;

         [[nodiscard]] RoutingNodeTriggerType getConnectedComponentTriggerType(Base::VertexHandle);
         Optional<Base::EdgeHandle> addEdge(const Base::VertexHandle& from, const Base::VertexHandle& to);
         Optional<SharedPtr<platform::InterruptDomain>> getEffectiveOwner(const Base::VertexHandle&);
      };

      SharedPtr<RoutingGraphBuilder> createRoutingGraphBuilder();
   }

   namespace platform {
      // Reserved for future use: a routable domain whose valid routes depend on the current
      // state of the routing graph being built (e.g. per-CPU affinity steering). No concrete
      // implementation exists yet; the constraint engine already checks for this type in
      // isEdgeAllowedImpl so that adding an implementation requires no framework changes.
      CRClass(ContextDependentRoutableDomain, public RoutableDomain) {
         public:
         [[nodiscard]] virtual bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter, const GraphBuilderBase<managed::RoutingGraph>& builder) const = 0;
      };
   }
}

#endif //CROCOS_INTERRUPT_ROUTING_GRAPH_H
