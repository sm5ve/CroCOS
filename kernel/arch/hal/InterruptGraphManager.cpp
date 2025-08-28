//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
#include <core/ds/Graph.h>
#include <core/utility.h>

namespace kernel::hal::interrupts {
    namespace topology {
        
#ifdef CROCOS_TESTING
        bool isDirty = false;
        Optional<TopologyGraph> cachedGraph;
        GraphBuilder<TopologyGraph>* builder_ptr = nullptr;
        GraphBuilder<TopologyGraph>& getBuilder() {
            if (!builder_ptr) {
                builder_ptr = new GraphBuilder<TopologyGraph>();
            }
            return *builder_ptr;
        }
        
        // Function to reset state between tests
        void resetTopologyState() {
            if (builder_ptr) {
                delete builder_ptr;
                builder_ptr = nullptr;
            }
            isDirty = false;
            cachedGraph = {};
        }
#else
        bool isDirty = false;
        WITH_GLOBAL_CONSTRUCTOR(Optional<TopologyGraph>, cachedGraph);
        WITH_GLOBAL_CONSTRUCTOR(GraphBuilder<TopologyGraph>, topologyBuilder);
        GraphBuilder<TopologyGraph>& getBuilder() {
            return topologyBuilder;
        }
#endif

        Optional<TopologyGraph>& getTopologyGraph() {
            if (isDirty || !cachedGraph.occupied()) {
                cachedGraph = getBuilder().build();
                isDirty = false;
            }
            return cachedGraph;
        }

        void registerDomain(SharedPtr<platform::InterruptDomain> domain) {
            getBuilder().addVertex(move(domain));
            isDirty = true;
        }

        void registerConnector(SharedPtr<platform::DomainConnector> connector) {
            auto& builder = getBuilder();
            auto source = builder.getVertexByLabel(connector -> getSource());
            auto target = builder.getVertexByLabel(connector -> getTarget());
            assert(source.occupied() && target.occupied(), "Must add interrupt domains before registering a connector between them");
            assert(connector -> getSource() -> instanceof(TypeID_v<platform::InterruptEmitter>), "Connector source must be an interrupt emitter");
            assert(connector -> getTarget() -> instanceof(TypeID_v<platform::InterruptReceiver>), "Connector target must be an interrupt receiver");
            builder.addEdge(*source, *target, move(connector));
            isDirty = true;
        }

        using ExclusiveConnectorMap = HashMap<managed::RoutingNodeLabel, SharedPtr<platform::DomainConnector>>;
#ifdef CROCOS_TESTING

#else
        WITH_GLOBAL_CONSTRUCTOR(ExclusiveConnectorMap, exclusiveConnectors);

        ExclusiveConnectorMap& getExclusiveConnectors() {
            return exclusiveConnectors;
        }
#endif

        bool registerExclusiveConnector(SharedPtr<platform::DomainConnector> connector) {
            auto& builder = getBuilder();
            auto source = builder.getVertexByLabel(connector -> getSource());
            auto target = builder.getVertexByLabel(connector -> getTarget());
            auto targetReceiver = crocos_dynamic_cast<platform::InterruptReceiver>(connector -> getTarget());
            assert(source.occupied() && target.occupied(), "Must add interrupt domains before registering a connector between them");
            assert(connector -> getSource() -> instanceof(TypeID_v<platform::InterruptEmitter>), "Connector source must be an interrupt emitter");
            assert(targetReceiver, "Connector target must be an interrupt receiver");
            builder.addEdge(*source, *target, move(connector));
            isDirty = true;
            bool wasSuccessful = true;
            for (size_t i = 0; i < targetReceiver -> getReceiverCount(); i++) {
                auto targetLabel = managed::RoutingNodeLabel(connector -> getTarget(), i);
                if (exclusiveConnectors.contains(targetLabel)) {
                    wasSuccessful = false;
                    continue;
                }
                exclusiveConnectors.insert(targetLabel, connector);
            }

            return wasSuccessful;
        }
    }

    namespace managed {
        size_t RoutingNodeLabel::hash() const {
            const size_t domainHash = DefaultHasher<SharedPtr<kernel::hal::interrupts::platform::InterruptDomain>>{}(this -> domain);
            const size_t indexHash = this -> index;

            return domainHash ^ (indexHash << 1);
        }

        SharedPtr<RoutingGraphBuilder> createRoutingGraphBuilder() {
            struct RoutingVertexSpec {
                RoutingNodeLabel label;
                int color;
            };
            
            Vector<RoutingVertexSpec> routingVertices;
            
            auto topologyGraph = topology::getTopologyGraph();
            if (!topologyGraph.occupied()) {
                return SharedPtr<RoutingGraphBuilder>();
            }

            for (const auto vertex : topologyGraph->vertices()) {
                auto domain = topologyGraph->getVertexLabel(vertex);
                
                if (domain->instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    const auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
                    for (size_t i = 0; i < receiver->getReceiverCount(); i++) {
                        routingVertices.push(RoutingVertexSpec{
                            RoutingNodeLabel(domain, i),
                            0
                        });
                    }
                }
                else if (domain->instanceof(TypeID_v<platform::InterruptEmitter>)) {
                    const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                    for (size_t i = 0; i < emitter->getEmitterCount(); i++) {
                        routingVertices.push(RoutingVertexSpec{
                            RoutingNodeLabel(domain, i),
                            0
                        });
                    }
                }
                else {
                    assertNotReached("Domain is neither receiver nor emitter - don't know what to do.");
                }
            }
            //Vertices in the routing graph are pairs (Domain, ind)
            //where ind is the index of an emitter if Domain is not a receiver, and
            //the index of a receiver otherwise. In the former case, the node is labeled as a device
            //and in the latter, it is labeled as an input.

            //For a fixed routing domain, we can prepopulate the edges coming out of it in advance - they are forced
            auto out = make_shared<RoutingGraphBuilder>(routingVertices, RoutingConstraint{});
            for (auto topVert : topologyGraph->vertices()) {
                auto domain = topologyGraph->getVertexLabel(topVert);
                if (auto fixedDomain = crocos_dynamic_cast<platform::FixedRoutingDomain>(domain)) {
                    for (const auto outgoingEdge : topologyGraph -> outgoingEdges(topVert)) {
                        const auto targetVertex = topologyGraph -> getTarget(outgoingEdge);
                        const auto targetDomain = topologyGraph -> getVertexLabel(targetVertex);
                        const auto connector = topologyGraph -> getEdgeLabel(outgoingEdge);
                        for (size_t sourceIndex = 0; sourceIndex < fixedDomain -> getReceiverCount(); sourceIndex++) {
                            const auto emitterIndex = fixedDomain -> getEmitterFor(sourceIndex);
                            if (const auto targetIndex = connector -> fromOutput(emitterIndex)) {
                                auto targetLabel = RoutingNodeLabel(targetDomain, *targetIndex);
                                auto sourceLabel = RoutingNodeLabel(domain, sourceIndex);
                                auto sourceBuilderVertex = out -> getVertexByLabel(sourceLabel);
                                auto targetBuilderVertex = out -> getVertexByLabel(targetLabel);
                                assert(sourceBuilderVertex.occupied() && targetBuilderVertex.occupied(), "Must have a vertex for each domain");
                                out -> addEdge(*sourceBuilderVertex, *targetBuilderVertex);
                            }
                        }
                    }
                }
                //If this is a device node, we can also immediately populate all edges
                else if (!domain -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                    assert(emitter, "Domain must be an emitter");
                    for (const auto outgoingEdge : topologyGraph -> outgoingEdges(topVert)) {
                        const auto targetVertex = topologyGraph -> getTarget(outgoingEdge);
                        const auto targetDomain = topologyGraph -> getVertexLabel(targetVertex);
                        const auto connector = topologyGraph -> getEdgeLabel(outgoingEdge);
                        for (size_t sourceIndex = 0; sourceIndex < emitter -> getEmitterCount(); sourceIndex++) {
                            if (auto targetIndex = connector -> fromOutput(sourceIndex)) {
                                auto targetLabel = RoutingNodeLabel(targetDomain, *targetIndex);
                                auto sourceLabel = RoutingNodeLabel(domain, sourceIndex);
                                auto sourceBuilderVertex = out -> getVertexByLabel(sourceLabel);
                                auto targetBuilderVertex = out -> getVertexByLabel(targetLabel);
                                out -> addEdge(*sourceBuilderVertex, *targetBuilderVertex);
                            }
                        }
                    }
                }
            }
            return out;
        }

        bool RoutingConstraint::isEdgeAllowed(const Builder &graph, const VertexHandle source, const VertexHandle target) {
            if (graph.getOutgoingEdgeCount(source) > 0) {
                return graph.hasEdge(source, target);
            }

            const auto sourceDomain = graph.getVertexLabel(source) -> domain;
            const auto sourceIndex = graph.getVertexLabel(source) -> index;
            const auto sourceType = graph.getVertexLabel(source) -> getType();
            const auto targetDomain = graph.getVertexLabel(target) -> domain;
            const auto targetIndex = graph.getVertexLabel(target) -> index;

            //This will only ever be called if we already know the topology graph is preconstructed from createRoutingGraphBuilder
            //so it is safe to dereference
            const auto& topologyGraph = *topology::getTopologyGraph();

            auto sourceTopologyVertex = topologyGraph.getVertexByLabel(sourceDomain);
            auto targetTopologyVertex = topologyGraph.getVertexByLabel(targetDomain);

            assert(sourceTopologyVertex.occupied() && targetTopologyVertex.occupied(), "Must have a topology vertex for each domain");
            if (const auto edge = topologyGraph.findEdge(*sourceTopologyVertex, *targetTopologyVertex)) {
                const auto connector = topologyGraph.getEdgeLabel(*edge);
                const auto targetLabel = graph.getVertexLabel(target);
                //If the target receiver is connected to by an exclusive connector, confirm that 'connector' is that exclusive connector
                if (topology::getExclusiveConnectors().contains(*targetLabel)) {
                    const auto exclusiveConnector = topology::getExclusiveConnectors().at(*targetLabel);
                    if (connector != exclusiveConnector) return false;
                }
                if (auto emitterIndex = connector->fromInput(targetIndex)) {
                    const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(sourceDomain);
                    if (!emitter) return false;
                    assert(*emitterIndex < (emitter -> getEmitterCount()), "Emitter index out of bounds");
                    if (sourceType == NodeType::Device) {
                        assert(!(emitter -> instanceof(TypeID_v<platform::InterruptReceiver>)), "Source type improperly set");
                        return (connector -> fromOutput(sourceIndex).occupied()) && (*connector -> fromOutput(sourceIndex) == targetIndex);
                    }
                    if (emitter -> instanceof(TypeID_v<platform::FreeRoutableDomain>)) {
                        return true;
                    }
                    if (const auto routableDomain = crocos_dynamic_cast<platform::ContextIndependentRoutableDomain>(emitter)) {
                        return routableDomain -> isRoutingAllowed(sourceIndex, *emitterIndex);
                    }
                    if (const auto routableDomain = crocos_dynamic_cast<platform::ContextDependentRoutableDomain>(emitter)) {
                        return routableDomain -> isRoutingAllowed(sourceIndex, *emitterIndex, graph);
                    }
                    if (const auto fixedDomain = crocos_dynamic_cast<platform::FixedRoutingDomain>(emitter)) {
                        auto expectedTarget = connector -> fromOutput(fixedDomain -> getEmitterFor(sourceIndex));
                        if (!expectedTarget) return false;
                        return *expectedTarget == targetIndex;
                    }
                    assertUnimplemented("Interrupt domain is both receiver and emitter, but not of a known subtype");
                }
            }
            return false;
        }

        IteratorRange<PotentialEdgeIterator<true>> RoutingConstraint::validEdgesFrom(const Builder &graph, const VertexHandle source) {
            using It = PotentialEdgeIterator<true>;
            
            const auto sourceDomain = graph.getVertexLabel(source)->domain;
            const auto sourceIndex = graph.getVertexLabel(source)->index;
            
            const auto& topologyGraph = *topology::getTopologyGraph();
            auto sourceTopologyVertex = topologyGraph.getVertexByLabel(sourceDomain);
            assert(sourceTopologyVertex.occupied(), "Source domain must exist in topology");
            
            const auto outgoingEdges = topologyGraph.outgoingEdges(*sourceTopologyVertex);
            auto begin = outgoingEdges.begin();
            auto end = outgoingEdges.end();
            
            return {
                It(sourceDomain, begin, end, 0, sourceIndex, &graph),
                It(sourceDomain, end, end, 0, sourceIndex, &graph)
            };
        }

        IteratorRange<PotentialEdgeIterator<false>> RoutingConstraint::validEdgesTo(const Builder &graph, const VertexHandle target) {
            using It = PotentialEdgeIterator<false>;

            const auto targetDomain = graph.getVertexLabel(target)->domain;
            const auto targetIndex = graph.getVertexLabel(target)->index;

            const auto& topologyGraph = *topology::getTopologyGraph();
            auto targetTopologyVertex = topologyGraph.getVertexByLabel(targetDomain);
            assert(targetTopologyVertex.occupied(), "Source domain must exist in topology");

            const auto incomingEdges = topologyGraph.incomingEdges(*targetTopologyVertex);
            auto begin = incomingEdges.begin();
            auto end = incomingEdges.end();

            return {
                It(targetDomain, begin, end, 0, targetIndex, &graph),
                It(targetDomain, end, end, 0, targetIndex, &graph)
            };
        }

        template<bool Forward>
        PotentialEdgeIterator<Forward>::PotentialEdgeIterator(const SharedPtr<platform::InterruptDomain>& domain, Iterator& itr,
            Iterator& end, const size_t index, size_t findex, const GraphBuilderBase<RoutingGraph>* g):
            currentConnector(itr), endConnector(end), currentIndex(index), fixedDomain(domain), fixedIndex(findex), graph(g) {
            assert(fixedDomain, "Fixed domain is null");
            advanceToValidState();
        }

        template<bool Forward>
        PotentialEdgeIterator<Forward>& PotentialEdgeIterator<Forward>::operator++() {
            advanceIntermediateState();
            advanceToValidState();
            return *this;
        }

        template<>
        void PotentialEdgeIterator<true>::advanceIntermediateState() {
            if (!currentConnector.operator!=(endConnector)){
                return;
            }
            auto& topGraph = *topology::getTopologyGraph();
            auto edge = currentConnector.operator*();
            auto targetVertex = topGraph.getTarget(edge);
            const auto& targetDomain = topGraph.getVertexLabel(targetVertex);
            const auto receiverDomain = crocos_dynamic_cast<platform::InterruptReceiver>(targetDomain);
            assert(receiverDomain, "Target domain must be a receiver");
            currentIndex++;
            if (currentIndex >= receiverDomain->getReceiverCount()) {
                currentIndex = 0;
                currentConnector.operator++();
            }
        }

        template<>
        void PotentialEdgeIterator<false>::advanceIntermediateState() {
            if (!currentConnector.operator!=(endConnector)){
                return;
            }
            auto& topGraph = *topology::getTopologyGraph();
            auto edge = currentConnector.operator*();
            auto fromVertex = topGraph.getSource(edge);
            const auto fromDomain = topGraph.getVertexLabel(fromVertex);
            const auto emitterDomain = crocos_dynamic_cast<platform::InterruptEmitter>(fromDomain);
            assert(emitterDomain, "Source domain must be an emitter");
            currentIndex++;
            //If fromDomain is not a device, we index over receivers
            if (const auto receiverDomain = crocos_dynamic_cast<platform::InterruptReceiver>(fromDomain)) {
                if (currentIndex >= receiverDomain -> getReceiverCount()) {
                    currentIndex = 0;
                    currentConnector.operator++();
                }
            }
            //If fromDomain is a device, then we're indexing over emitters
            else if (currentIndex >= emitterDomain->getEmitterCount()) {
                currentIndex = 0;
                currentConnector.operator++();
            }
        }

        template<>
        bool PotentialEdgeIterator<false>::isValidIntermediateState() const {
            if (!graph) return false;
            auto& topologyGraph = *topology::getTopologyGraph();
            auto targetLabel = RoutingNodeLabel(fixedDomain, fixedIndex);
            auto sourceDomain = topologyGraph.getVertexLabel(topologyGraph.getSource(*currentConnector));
            auto sourceLabel = RoutingNodeLabel(sourceDomain, currentIndex);

            auto sourceVertex = graph -> getVertexByLabel(sourceLabel);
            auto targetVertex = graph -> getVertexByLabel(targetLabel);
            if (!(sourceVertex.occupied() && targetVertex.occupied())) return false;
            return RoutingConstraint::isEdgeAllowed(*graph, *sourceVertex, *targetVertex);
        }

        template<>
        bool PotentialEdgeIterator<true>::isValidIntermediateState() const {
            if (!graph) return false;
            auto& topologyGraph = *topology::getTopologyGraph();
            auto sourceLabel = RoutingNodeLabel(fixedDomain, fixedIndex);
            auto targetDomain = topologyGraph.getVertexLabel(topologyGraph.getTarget(*currentConnector));
            auto targetLabel = RoutingNodeLabel(targetDomain, currentIndex);

            auto sourceVertex = graph -> getVertexByLabel(sourceLabel);
            auto targetVertex = graph -> getVertexByLabel(targetLabel);
            if (!(sourceVertex.occupied() && targetVertex.occupied())) return false;
            return RoutingConstraint::isEdgeAllowed(*graph, *sourceVertex, *targetVertex);
        }


        template<bool Forward>
        void PotentialEdgeIterator<Forward>::advanceToValidState() {
            while (currentConnector.operator!=(endConnector) && !isValidIntermediateState()) {
                advanceIntermediateState();
            }
        }

        template<bool Forward>
        BuilderVertexHandle<RoutingGraph> PotentialEdgeIterator<Forward>::operator*() const {
            assert(this -> currentConnector != this -> endConnector, "Tried to dereference end connector");
            // Find the target domain and create a RoutingNodeLabel for it
            auto& topologyGraph = *topology::getTopologyGraph();
            auto edge = currentConnector.operator*();
            
            SharedPtr<platform::InterruptDomain> targetDomain;
            
            if constexpr (Forward) {
                auto targetVertex = topologyGraph.getTarget(edge);
                targetDomain = topologyGraph.getVertexLabel(targetVertex);
            } else {
                auto sourceVertex = topologyGraph.getSource(edge);
                targetDomain = topologyGraph.getVertexLabel(sourceVertex);
            }
            
            const RoutingNodeLabel targetLabel(targetDomain, currentIndex);
            auto vertex = this->graph->getVertexByLabel(targetLabel);
            assert(vertex.occupied(), "Target vertex must exist in routing graph");
            return *vertex;
        }
    }

    namespace platform {
        AffineConnector::AffineConnector(SharedPtr<InterruptDomain> src, SharedPtr<InterruptDomain> tgt, size_t off) : DomainConnector(src, tgt), offset(off) {
            auto emitter = crocos_dynamic_cast<InterruptEmitter>(src);
            auto receiver = crocos_dynamic_cast<InterruptReceiver>(tgt);
            assert(emitter, "Source domain must be an emitter");
            assert(receiver, "Target domain must be a receiver");
            assert(emitter -> getEmitterCount() + offset <= receiver -> getReceiverCount(), "Offset is out of bounds");
        }

        Optional<DomainInputIndex> AffineConnector::fromOutput(DomainOutputIndex index) const {
            return index + offset;
        }

        Optional<DomainOutputIndex> AffineConnector::fromInput(DomainInputIndex index) const {
            return index - offset;
        }

        CPUInterruptVectorFile::CPUInterruptVectorFile(size_t w) : width(w) {}

        size_t CPUInterruptVectorFile::getReceiverCount() {
            return width;
        }

        SharedPtr<CPUInterruptVectorFile> vectorFile;

        const SharedPtr<CPUInterruptVectorFile> getCPUInterruptVectors(){
            return vectorFile;
        }

        bool setupCPUInterruptVectorFile(size_t size) {
            static bool initialized = false;
            if (initialized) return false;
            vectorFile = make_shared<CPUInterruptVectorFile>(size);
            topology::registerDomain(vectorFile);
            initialized = true;
            return true;
        }

    }
}

size_t DefaultHasher<kernel::hal::interrupts::managed::RoutingNodeLabel>::operator()(const kernel::hal::interrupts::managed::RoutingNodeLabel& label) const {
    return label.hash();
}

// Explicit template instantiations to force code generation
template class kernel::hal::interrupts::managed::PotentialEdgeIterator<true>;
template class kernel::hal::interrupts::managed::PotentialEdgeIterator<false>;