//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
#include <core/ds/Graph.h>
#include <core/utility.h>
#include <core/algo/GraphAlgorithms.h>

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
        ExclusiveConnectorMap* exclusiveConnectors;

        ExclusiveConnectorMap& getExclusiveConnectors() {
            if (exclusiveConnectors == nullptr) {
                exclusiveConnectors = new ExclusiveConnectorMap();
            }
            return *exclusiveConnectors;
        }

        // Function to reset state between tests
        void resetTopologyState() {
            if (builder_ptr) {
                delete builder_ptr;
                builder_ptr = nullptr;
            }
            if (exclusiveConnectors) {
                delete exclusiveConnectors;
                exclusiveConnectors = nullptr;
            }
            isDirty = false;
            cachedGraph = {};
        }
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
                if (getExclusiveConnectors().contains(targetLabel)) {
                    wasSuccessful = false;
                    continue;
                }
                getExclusiveConnectors().insert(targetLabel, connector);
            }

            return wasSuccessful;
        }
    }

    namespace managed {
        size_t RoutingNodeLabel::hash() const {
            const size_t domainHash = DefaultHasher<SharedPtr<kernel::hal::interrupts::platform::InterruptDomain>>{}(this -> domain());
            const size_t indexHash = this -> index();

            return domainHash ^ (indexHash << 1);
        }

        SharedPtr<RoutingGraphBuilder> createRoutingGraphBuilder() {
            struct RoutingVertexSpec {
                RoutingNodeLabel label;
                RoutingNodeTriggerType color;
            };
            
            Vector<RoutingVertexSpec> routingVertices;
            
            auto topologyGraph = topology::getTopologyGraph();
            if (!topologyGraph.occupied()) {
                return SharedPtr<RoutingGraphBuilder>();
            }

            for (const auto vertex : topologyGraph->vertices()) {
                auto domain = topologyGraph->getVertexLabel(vertex);
                auto configurableTriggerDomain = crocos_dynamic_cast<platform::ConfigurableActivationTypeDomain>(domain);
                if (domain->instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    const auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
                    for (size_t i = 0; i < receiver->getReceiverCount(); i++) {
                        auto triggerType = TRIGGER_UNDETERMINED;
                        if (configurableTriggerDomain) {
                            auto activationType = configurableTriggerDomain -> getActivationType(i);
                            if (activationType) {
                                if (isLevelTriggered(*activationType)) {
                                    triggerType = TRIGGER_LEVEL;
                                }
                                else {
                                    triggerType = TRIGGER_EDGE;
                                }
                            }
                        }
                        routingVertices.push(RoutingVertexSpec{
                            RoutingNodeLabel(domain, i),
                            triggerType
                        });
                    }
                }
                else if (domain->instanceof(TypeID_v<platform::InterruptEmitter>)) {
                    const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                    for (size_t i = 0; i < emitter->getEmitterCount(); i++) {
                        auto triggerType = TRIGGER_UNDETERMINED;
                        if (configurableTriggerDomain) {
                            auto activationType = configurableTriggerDomain -> getActivationType(i);
                            if (activationType) {
                                if (isLevelTriggered(*activationType)) {
                                    triggerType = TRIGGER_LEVEL;
                                }
                                else {
                                    triggerType = TRIGGER_EDGE;
                                }
                            }
                        }
                        routingVertices.push(RoutingVertexSpec{
                            RoutingNodeLabel(domain, i),
                            triggerType
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
            auto out = make_shared<RoutingGraphBuilder>(routingVertices);
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

        bool RoutingConstraint::isEdgeAllowedImpl(const Builder &graph, const VertexHandle source, const VertexHandle target, bool checkTriggerType) {
            auto& routingBuilder = RoutingGraphBuilder::fromGenericBuilder(graph);
            if (graph.getOutgoingEdgeCount(source) > 0) {
                return graph.hasEdge(source, target);
            }

            const auto sourceDomain = graph.getVertexLabel(source) -> domain();
            const auto sourceIndex = graph.getVertexLabel(source) -> index();
            const auto sourceType = graph.getVertexLabel(source) -> getType();
            const auto targetDomain = graph.getVertexLabel(target) -> domain();
            const auto targetIndex = graph.getVertexLabel(target) -> index();

            const auto sourceActivationType = routingBuilder.getConnectedComponentTriggerType(source);
            const auto targetActivationType = routingBuilder.getConnectedComponentTriggerType(target);
            //Allowed connections are LEVEL -> LEVEL, LEVEL -> UNDETERMINED, and any mix of EDGE and UNDETERMINED
            if (checkTriggerType) {
                if (targetActivationType == TRIGGER_LEVEL) {
                    //Except we do allow connecting an undetermined device to a level-triggered source
                    //We then conclude the device wants level-triggered interrupts and mark it as such.
                    if (sourceActivationType != TRIGGER_LEVEL && sourceDomain -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                        return false;
                    }
                }
                if (targetActivationType == TRIGGER_EDGE) {
                    if (sourceActivationType == TRIGGER_LEVEL){
                        return false;
                    }
                }
            }
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

        IteratorRange<PotentialEdgeIterator<true>> RoutingConstraint::validEdgesFromImpl(const Builder &graph, const VertexHandle source, bool checkTriggerType) {
            using It = PotentialEdgeIterator<true>;
            
            const auto sourceDomain = graph.getVertexLabel(source)->domain();
            const auto sourceIndex = graph.getVertexLabel(source)->index();
            
            const auto& topologyGraph = *topology::getTopologyGraph();
            auto sourceTopologyVertex = topologyGraph.getVertexByLabel(sourceDomain);
            assert(sourceTopologyVertex.occupied(), "Source domain must exist in topology");
            
            const auto outgoingEdges = topologyGraph.outgoingEdges(*sourceTopologyVertex);
            auto begin = outgoingEdges.begin();
            auto end = outgoingEdges.end();
            
            return {
                It(sourceDomain, begin, end, 0, sourceIndex, &graph, checkTriggerType),
                It(sourceDomain, end, end, 0, sourceIndex, &graph, checkTriggerType)
            };
        }

        IteratorRange<PotentialEdgeIterator<false>> RoutingConstraint::validEdgesToImpl(const Builder &graph, const VertexHandle target, bool checkTriggerType) {
            using It = PotentialEdgeIterator<false>;

            const auto targetDomain = graph.getVertexLabel(target)->domain();
            const auto targetIndex = graph.getVertexLabel(target)->index();

            const auto& topologyGraph = *topology::getTopologyGraph();
            auto targetTopologyVertex = topologyGraph.getVertexByLabel(targetDomain);
            assert(targetTopologyVertex.occupied(), "Source domain must exist in topology");

            const auto incomingEdges = topologyGraph.incomingEdges(*targetTopologyVertex);
            auto begin = incomingEdges.begin();
            auto end = incomingEdges.end();

            return {
                It(targetDomain, begin, end, 0, targetIndex, &graph, checkTriggerType),
                It(targetDomain, end, end, 0, targetIndex, &graph, checkTriggerType)
            };
        }

        bool RoutingConstraint::isEdgeAllowed(const Builder &graph, VertexHandle source, VertexHandle target) {
            return isEdgeAllowedImpl(graph, source, target, true);
        }

        IteratorRange<PotentialEdgeIterator<true> > RoutingConstraint::validEdgesFrom(const Builder &graph, VertexHandle source) {
            return validEdgesFromImpl(graph, source, true);
        }

        IteratorRange<PotentialEdgeIterator<false> > RoutingConstraint::validEdgesTo(const Builder &graph, VertexHandle target) {
            return validEdgesToImpl(graph, target, true);
        }

        template<bool Forward>
        PotentialEdgeIterator<Forward>::PotentialEdgeIterator(const SharedPtr<platform::InterruptDomain>& domain, Iterator& itr,
            Iterator& end, const size_t index, size_t findex, const GraphBuilderBase<RoutingGraph>* g, bool c):
            currentConnector(itr), endConnector(end), currentIndex(index), fixedDomain(domain), fixedIndex(findex), graph(g),
            checkTriggerType(c) {
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
            return RoutingConstraint::isEdgeAllowedImpl(*graph, *sourceVertex, *targetVertex, checkTriggerType);
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

        template<typename VertexContainer>
        RoutingGraphBuilder::RoutingGraphBuilder(const VertexContainer &vertices) : Base(vertices, RoutingConstraint{}){}

        RoutingNodeTriggerType RoutingGraphBuilder::getConnectedComponentTriggerType(Base::VertexHandle v) const{
            while (auto e = _getFirstEdgeFromVertex(v)) {
                v = getEdgeTarget(*e);
            }
            return *getVertexColor(v);
        }

        void RoutingGraphBuilder::setConnectedComponentTriggerType(Base::VertexHandle v, RoutingNodeTriggerType type) {
            while (auto e = _getFirstEdgeFromVertex(v)) {
                v = getEdgeTarget(*e);
            }
            _setVertexColor(v, type);
        }

        const RoutingGraphBuilder &RoutingGraphBuilder::fromGenericBuilder(const RoutingConstraint::Builder & b) {
            return static_cast<const RoutingGraphBuilder&>(Base::fromGenericBuilder(b));
        }

        Optional<RoutingGraph> RoutingGraphBuilder::build() {
            const auto topologyGraph = *topology::getTopologyGraph();
            const auto domainsByTopologicalOrder = algorithm::graph::topologicalSort(topologyGraph);
            for (int i = static_cast<int>(domainsByTopologicalOrder.getSize()) - 1; i >= 0; --i) {
                auto d = topologyGraph.getVertexLabel(domainsByTopologicalOrder[i]);
                if (auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(d)) {
                    for (size_t j = 0; j < receiver -> getReceiverCount(); j++) {
                        auto routingNodeLabel = RoutingNodeLabel(d, j);
                        auto vertex = getVertexByLabel(routingNodeLabel);
                        assert(vertex.occupied(), "Vertex must exist in routing graph");
                        auto nextInPath = _getFirstEdgeFromVertex(*vertex);
                        if (nextInPath.occupied()) {
                            auto nextVertex = getEdgeTarget(*nextInPath);
                            if (*getVertexColor(nextVertex) == TRIGGER_UNDETERMINED) {
                                _setVertexColor(nextVertex, TRIGGER_EDGE); //Use edge trigger mode as a default
                            }
                            _setVertexColor(*vertex, getVertexColor(nextVertex));
                        }
                    }
                }
                else if (auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(d)) {
                    for (size_t j = 0; j < emitter -> getEmitterCount(); j++) {
                        auto routingNodeLabel = RoutingNodeLabel(d, j);
                        auto vertex = getVertexByLabel(routingNodeLabel);
                        assert(vertex.occupied(), "Vertex must exist in routing graph");
                        auto nextInPath = _getFirstEdgeFromVertex(*vertex);
                        if (nextInPath.occupied()) {
                            auto nextVertex = getEdgeTarget(*nextInPath);
                            _setVertexColor(*vertex, getVertexColor(nextVertex));
                        }
                    }
                }
                else {
                    assertNotReached("Interrupt domain must at least be emitter or receiver");
                }
            }
            return Base::build();
        }


        Optional<RoutingGraphBuilder::EdgeHandle> RoutingGraphBuilder::addEdge(const Base::VertexHandle &from, const Base::VertexHandle &to) {
            const auto out =  Base::addEdge(from, to);
            if (out.occupied()) {
                const auto sourceTriggerType = *getVertexColor(from);
                const auto targetTriggerType = getConnectedComponentTriggerType(to);
                if (targetTriggerType == TRIGGER_UNDETERMINED && sourceTriggerType != TRIGGER_UNDETERMINED) {
                    setConnectedComponentTriggerType(to, sourceTriggerType);
                }
            }
            return out;
        }

        bool RoutingGraphBuilder::isEdgeAllowedIgnoringTriggerType(Base::VertexHandle source, Base::VertexHandle target) const {
            validateVertexHandle(source);
            validateVertexHandle(target);
            if (hasEdge(source, target)) return false;
            return RoutingConstraint::isEdgeAllowedImpl(asBase(), source, target, false);
        }

        RoutingGraphBuilder::FilteredPotentialEdgeIterator<false> RoutingGraphBuilder::validEdgesToIgnoringTriggerType(VertexHandle target) const {
            auto baseIterator = RoutingConstraint::validEdgesToImpl(asBase(), target, false);
            return FilteredPotentialEdgeIterator(move(baseIterator), *this, target);
        }

        RoutingGraphBuilder::FilteredPotentialEdgeIterator<true> RoutingGraphBuilder::validEdgesFromIgnoringTriggerType(VertexHandle target) const {
            auto baseIterator = RoutingConstraint::validEdgesFromImpl(asBase(), target, false);
            return FilteredPotentialEdgeIterator(move(baseIterator), *this, target);
        }
    }

    namespace platform {
        AffineConnector::AffineConnector(SharedPtr<InterruptDomain> src, SharedPtr<InterruptDomain> tgt, size_t off, size_t s, size_t w) : DomainConnector(src, tgt), offset(off), start(s), width(w) {
            auto emitter = crocos_dynamic_cast<InterruptEmitter>(src);
            auto receiver = crocos_dynamic_cast<InterruptReceiver>(tgt);
            assert(emitter, "Source domain must be an emitter");
            assert(receiver, "Target domain must be a receiver");
            assert(start + offset + width <= receiver -> getReceiverCount(), "Offset is out of bounds");
            assert(start + width <= emitter -> getEmitterCount(), "Connector too wide");
        }

        Optional<DomainInputIndex> AffineConnector::fromOutput(DomainOutputIndex index) const {
            if (index < start) return {};
            if (index >= start + width) return {};
            return index + offset;
        }

        Optional<DomainOutputIndex> AffineConnector::fromInput(DomainInputIndex index) const {
            auto toReturn = index - offset;
            if (toReturn < start) return {};
            if (toReturn >= start + width) return {};
            return toReturn;
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