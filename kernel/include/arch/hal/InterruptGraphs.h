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

namespace kernel::hal::interrupts {
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

         virtual Optional<DomainInputIndex> fromOutput(DomainOutputIndex) = 0;
         virtual Optional<DomainOutputIndex> fromInput(DomainInputIndex) = 0;
      };
   }

   namespace topology {
      void registerDomain(SharedPtr<platform::InterruptDomain> domain);
      void registerConnector(SharedPtr<platform::DomainConnector> connector);
      
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
         SharedPtr<platform::InterruptDomain> domain;
         size_t index;
         NodeType type;
         friend struct RoutingConstraint;
      public:
         RoutingNodeLabel(SharedPtr<platform::InterruptDomain>&& d, size_t ind, NodeType t) : domain(move(d)), index(ind), type(t) {}
         RoutingNodeLabel(const SharedPtr<platform::InterruptDomain>& d, size_t ind, NodeType t) : domain(d), index(ind), type(t) {}
         bool operator==(const RoutingNodeLabel& other) const = default;
         size_t hash() const;
         
#ifdef CROCOS_TESTING
         // Accessors for testing only
         const SharedPtr<platform::InterruptDomain>& getDomain() const { return domain; }
         size_t getIndex() const { return index; }
         NodeType getType() const { return type; }
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
      using RoutingVertexConfig = GraphProperties::ColoredLabeledVertex<int, RoutingNodeLabel>;
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
         friend struct RoutingConstraint;
         struct None{};
         conditional_t<Forward, bool, None> trivialIterator;
         PotentialEdgeIterator(const SharedPtr<platform::InterruptDomain>& domain, Iterator& itr,
            Iterator& end, size_t index, size_t findex, const GraphBuilderBase<RoutingGraph>* g);
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
         static bool isEdgeAllowed(const Builder& graph, VertexHandle source, VertexHandle target) ;
         static IteratorRange<PotentialEdgeIterator<true>> validEdgesFrom(const Builder& graph, VertexHandle source) ;
         static IteratorRange<PotentialEdgeIterator<false>> validEdgesTo(const Builder& graph, VertexHandle target) ;
      };

      using RoutingGraphBuilder = RestrictedGraphBuilder<RoutingGraph, RoutingConstraint>;
      
      SharedPtr<RoutingGraphBuilder> createRoutingGraphBuilder();
   }
}


#endif //CROCOS_INTERRUPTS_H
