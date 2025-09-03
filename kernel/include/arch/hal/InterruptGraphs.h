//
// Created by Spencer Martin on 4/10/25.
//

#ifndef CROCOS_INTERRUPTS_H
#define CROCOS_INTERRUPTS_H

#include <core/GraphBuilder.h>
#include <core/algo/GraphPredicates.h>
#include <core/Object.h>
#include <core/ds/SmartPointer.h>
#include <core/Hasher.h>

#include "interrupts.h"

namespace kernel::hal::interrupts {
   enum InterruptLineActivationType : uint8_t{
      LEVEL_LOW = 0b00,
      LEVEL_HIGH = 0b01,
      EDGE_LOW = 0b10,
      EDGE_HIGH = 0b11
  };

   constexpr InterruptLineActivationType activationTypeForLevelAndTriggerMode(const bool activeHigh, const bool edgeTriggered) {
      uint8_t bits = 0;
      if (activeHigh) bits |= 1;
      if (edgeTriggered) bits |= 2;
      return static_cast<InterruptLineActivationType>(bits);
   }

   constexpr bool isLevelTriggered(const InterruptLineActivationType type){
      return type == LEVEL_LOW || type == LEVEL_HIGH;
   }

   constexpr bool isEdgeTriggered(const InterruptLineActivationType type){
      return !isLevelTriggered(type);
   }

   constexpr bool isLowTriggered(const InterruptLineActivationType type){
      return type == LEVEL_LOW || type == EDGE_LOW;
   }

   constexpr bool isHighTriggered(const InterruptLineActivationType type){
      return !isLowTriggered(type);
   }

   namespace platform {
      CRClass(InterruptDomain) {

      };

      //Does it always make sense to index receivers and emitters from 0? Should it always be contiguous?
      CRClass(InterruptReceiver) {
      public:
         virtual size_t getReceiverCount() = 0;
      };

      CRClass(InterruptEmitter) {
      public:
         virtual size_t getEmitterCount() = 0;
      };

      CRClass(RoutableDomain, public InterruptReceiver, public InterruptEmitter) {
      public:
         virtual bool routeInterrupt(size_t fromReceiver, size_t toEmitter) = 0;
      };

      CRClass(FreeRoutableDomain, public RoutableDomain) {};

      CRClass(ContextIndependentRoutableDomain, public RoutableDomain) {
      public:
         [[nodiscard]] virtual bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter) const = 0;
      };

      CRClass(FixedRoutingDomain, public InterruptReceiver, public InterruptEmitter) {
      public:
         [[nodiscard]] virtual size_t getEmitterFor(size_t receiver) const = 0;
      };

      CRClass(MaskableDomain) {
      public:
         [[nodiscard]] virtual bool isReceiverMasked(size_t receiver) const = 0;
         virtual void setReceiverMask(size_t receiver, bool shouldMask) = 0;
      };

      CRClass(ConfigurableActivationTypeDomain) {
      public:
         virtual void setActivationType(size_t receiver, InterruptLineActivationType type) = 0;
         [[nodiscard]] virtual Optional<InterruptLineActivationType> getActivationType(size_t receiver) const = 0;
      };

      CRClass(CPUInterruptVectorFile, public InterruptDomain, public InterruptReceiver) {
         size_t width;
      public:
         CPUInterruptVectorFile(size_t width);
         [[nodiscard]] virtual size_t getReceiverCount() override;
      };

      using DomainInputIndex = size_t;
      using DomainOutputIndex = size_t;

      class DomainConnector {
      protected:
         SharedPtr<InterruptDomain> source;
         SharedPtr<InterruptDomain> target;
      public:
         DomainConnector(SharedPtr<InterruptDomain> src, SharedPtr<InterruptDomain> tgt) 
            : source(src), target(tgt) {}
         SharedPtr<InterruptDomain> getSource(){return source;}
         SharedPtr<InterruptDomain> getTarget(){return target;}
         virtual ~DomainConnector() = default;

         virtual Optional<DomainInputIndex> fromOutput(DomainOutputIndex) const = 0;
         virtual Optional<DomainOutputIndex> fromInput(DomainInputIndex) const = 0;
      };

      class AffineConnector : public DomainConnector {
         const size_t offset;
         const size_t start;
         const size_t width;
      public:
         AffineConnector(SharedPtr<InterruptDomain> src, SharedPtr<InterruptDomain> tgt, size_t offset, size_t start, size_t width);
         Optional<DomainInputIndex> fromOutput(DomainOutputIndex index) const override;
         Optional<DomainOutputIndex> fromInput(DomainInputIndex index) const override;
      };

      const SharedPtr<CPUInterruptVectorFile> getCPUInterruptVectors();
      bool setupCPUInterruptVectorFile(size_t size);
   }

   namespace topology {
      void registerDomain(SharedPtr<platform::InterruptDomain> domain);
      void registerConnector(SharedPtr<platform::DomainConnector> connector);
      bool registerExclusiveConnector(SharedPtr<platform::DomainConnector> connector);
      
      using TopologyVertexLabel = GraphProperties::LabeledVertex<SharedPtr<platform::InterruptDomain>>;
      using TopologyEdgeLabel = GraphProperties::LabeledEdge<SharedPtr<platform::DomainConnector>>;
      using TopologyGraphStructure = GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph, GraphPredicates::DirectedAcyclic>;
      using TopologyGraph = Graph<TopologyVertexLabel, TopologyEdgeLabel, TopologyGraphStructure>;
      
      Optional<TopologyGraph>& getTopologyGraph();
      
#ifdef CROCOS_TESTING
      void resetTopologyState();
#endif
   }

   namespace managed {
      enum class NodeType { Device, Input };
      struct RoutingConstraint;
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
struct DefaultHasher<kernel::hal::interrupts::managed::RoutingNodeLabel> {
   size_t operator()(const kernel::hal::interrupts::managed::RoutingNodeLabel& label) const;
};

namespace kernel::hal::interrupts {
   namespace managed {
      enum RoutingNodeTriggerType{
         TRIGGER_LEVEL = 0,
         TRIGGER_EDGE = 1,
         TRIGGER_UNDETERMINED = 2
     };

      using RoutingVertexConfig = GraphProperties::ColoredLabeledVertex<RoutingNodeTriggerType, RoutingNodeLabel>;
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
         [[nodiscard]] bool isValidIntermediateState() const;
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

         [[nodiscard]] bool isEdgeAllowedIgnoringTriggerType(Base::VertexHandle source, Base::VertexHandle target) const;
         [[nodiscard]] FilteredPotentialEdgeIterator<true> validEdgesFromIgnoringTriggerType(VertexHandle source) const;
         [[nodiscard]] FilteredPotentialEdgeIterator<false> validEdgesToIgnoringTriggerType(VertexHandle target) const;
      public:
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
         using Base::getVertices;
         using Base::getVertex;

         [[nodiscard]] RoutingNodeTriggerType getConnectedComponentTriggerType(Base::VertexHandle) const;
         Optional<Base::EdgeHandle> addEdge(const Base::VertexHandle& from, const Base::VertexHandle& to);
      };
      
      SharedPtr<RoutingGraphBuilder> createRoutingGraphBuilder();
   }

   namespace platform {
      CRClass(ContextDependentRoutableDomain, public RoutableDomain) {
         public:
         [[nodiscard]] virtual bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter, const GraphBuilderBase<managed::RoutingGraph>& builder) const = 0;
      };
   }
}


#endif //CROCOS_INTERRUPTS_H
