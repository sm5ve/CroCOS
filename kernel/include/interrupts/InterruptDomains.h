//
// Hardware-facing interrupt domain abstractions: activation types, platform domain
// base classes, connectors, and the topology graph interface.
//
// Include this header in device drivers and hardware bring-up code that registers
// domains and connects them without needing the routing graph types.
//

#ifndef CROCOS_INTERRUPT_DOMAINS_H
#define CROCOS_INTERRUPT_DOMAINS_H

#include <core/GraphBuilder.h>
#include <core/algo/GraphPredicates.h>
#include <core/Object.h>
#include <core/ds/SmartPointer.h>
#include <core/Hasher.h>

#include <arch.h>

namespace kernel::interrupts {
   enum class InterruptLineActivationType : uint8_t{
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
      return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::LEVEL_HIGH;
   }

   constexpr bool isEdgeTriggered(const InterruptLineActivationType type){
      return !isLevelTriggered(type);
   }

   constexpr bool isLowTriggered(const InterruptLineActivationType type){
      return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::EDGE_LOW;
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
         virtual size_t getReceiverCount() const = 0;
      };

      CRClass(InterruptEmitter) {
      public:
         virtual size_t getEmitterCount() const = 0;
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

      //TODO make some sort of configurable/fixed activation type emitter

      CRClass(EOIDomain) {
      public:
         virtual void issueEOI() = 0;
      };

      CRClass(CPUInterruptVectorFile, public InterruptDomain, public InterruptReceiver) {
         size_t width;
      public:
         CPUInterruptVectorFile(size_t width);
         [[nodiscard]] size_t getReceiverCount() const override;
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

         // Map an emitter output index to the corresponding receiver input index on the target.
         virtual Optional<DomainInputIndex> emitterToReceiver(DomainOutputIndex) const = 0;
         // Map a receiver input index on the target back to the emitter output index on the source.
         virtual Optional<DomainOutputIndex> receiverToEmitter(DomainInputIndex) const = 0;
      };

      class AffineConnector : public DomainConnector {
         const size_t offset;
         const size_t start;
         const size_t width;
      public:
         AffineConnector(SharedPtr<InterruptDomain> src, SharedPtr<InterruptDomain> tgt, size_t offset, size_t start, size_t width);
         Optional<DomainInputIndex> emitterToReceiver(DomainOutputIndex index) const override;
         Optional<DomainOutputIndex> receiverToEmitter(DomainInputIndex index) const override;
      };

      const SharedPtr<CPUInterruptVectorFile> getCPUInterruptVectors();
      bool setupCPUInterruptVectorFile(size_t size);
   }

   namespace topology {
      void registerDomain(SharedPtr<platform::InterruptDomain> domain);
      void registerConnector(SharedPtr<platform::DomainConnector> connector);
      bool registerExclusiveConnector(SharedPtr<platform::DomainConnector> connector);

      // Ergonomic helpers — connect all emitter outputs of src to a contiguous range of
      // receiver inputs on tgt starting at targetOffset (default 0).
      void connectAllOutputs(SharedPtr<platform::InterruptDomain> src,
                             SharedPtr<platform::InterruptDomain> tgt,
                             size_t targetOffset = 0);
      bool connectAllOutputsExclusive(SharedPtr<platform::InterruptDomain> src,
                                      SharedPtr<platform::InterruptDomain> tgt,
                                      size_t targetOffset = 0);

      // Ergonomic helpers — connect the single emitter output of src to targetInput on tgt.
      // Asserts that src has exactly one emitter.
      void connectSingleOutput(SharedPtr<platform::InterruptDomain> src,
                               SharedPtr<platform::InterruptDomain> tgt,
                               size_t targetInput);
      bool connectSingleOutputExclusive(SharedPtr<platform::InterruptDomain> src,
                                        SharedPtr<platform::InterruptDomain> tgt,
                                        size_t targetInput);

      using TopologyVertexLabel = GraphProperties::LabeledVertex<SharedPtr<platform::InterruptDomain>>;
      using TopologyEdgeLabel = GraphProperties::LabeledEdge<SharedPtr<platform::DomainConnector>>;
      using TopologyGraphStructure = GraphProperties::StructureModifier<GraphProperties::Directed, GraphProperties::SimpleGraph, GraphPredicates::DirectedAcyclic>;
      using TopologyGraph = Graph<TopologyVertexLabel, TopologyEdgeLabel, TopologyGraphStructure>;
      using TopologicalOrderMap = HashMap<SharedPtr<platform::InterruptDomain>, size_t>;

      Optional<TopologyGraph>& getTopologyGraph();
      Vector<SharedPtr<platform::InterruptDomain>>& topologicallySortedDomains();
      TopologicalOrderMap& topologicalOrderMap();
      void releaseCachedTopologicalOrdering();

#ifdef CROCOS_TESTING
      void resetTopologyState();
#endif
   }
}

#endif //CROCOS_INTERRUPT_DOMAINS_H
