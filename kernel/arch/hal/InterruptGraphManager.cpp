//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
#include <core/ds/Graph.h>
#include <core/utility.h>

namespace kernel::hal::interrupts {
    namespace topology {
        static bool isDirty = false;
        static Optional<TopologyGraph> cachedGraph;
        
#ifdef CROCOS_TESTING
        static GraphBuilder<TopologyGraph>* builder_ptr = nullptr;
        static GraphBuilder<TopologyGraph>& getBuilder() {
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
        WITH_GLOBAL_CONSTRUCTOR(GraphBuilder<TopologyGraph>, topologyBuilder);
        static GraphBuilder<TopologyGraph>& getBuilder() {
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
    }

    namespace managed {
        size_t RoutingNodeLabel::hash() const {
            const size_t domainHash = DefaultHasher<SharedPtr<kernel::hal::interrupts::platform::InterruptDomain>>{}(this -> domain);
            const size_t indexHash = this -> index;
            const size_t typeHash = static_cast<size_t>(this -> type);

            return domainHash ^ (indexHash << 1) ^ (typeHash << 2);
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
                            RoutingNodeLabel(domain, i, NodeType::Input),
                            0
                        });
                    }
                }
                
                if (domain->instanceof(TypeID_v<platform::InterruptEmitter>) && 
                    !domain->instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                    for (size_t i = 0; i < emitter->getEmitterCount(); i++) {
                        routingVertices.push(RoutingVertexSpec{
                            RoutingNodeLabel(domain, i, NodeType::Device),
                            0
                        });
                    }
                }
            }

            return make_shared<RoutingGraphBuilder>(routingVertices, RoutingConstraint{});
        }

        bool RoutingConstraint::isEdgeAllowed(const Builder &graph, VertexHandle source, VertexHandle target) {
            const auto sourceDomain = graph.getVertexLabel(source) -> domain;
            const auto sourceIndex = graph.getVertexLabel(source) -> index;
            const auto sourceType = graph.getVertexLabel(source) -> type;
            const auto targetDomain = graph.getVertexLabel(target) -> domain;
            const auto targetIndex = graph.getVertexLabel(target) -> index;

            //This will only ever be called if we already know the topology graph is preconstructed from createRoutingGraphBuilder
            //so it is safe to dereference
            const auto& topologyGraph = *topology::getTopologyGraph();

            auto sourceTopologyVertex = topologyGraph.getVertexByLabel(sourceDomain);
            auto targetTopologyVertex = topologyGraph.getVertexByLabel(targetDomain);

            assert(sourceTopologyVertex.occupied() && targetTopologyVertex.occupied(), "Must have a topology vertex for each domain");

            for (const auto edge : topologyGraph.outgoingEdges(*sourceTopologyVertex)) {
                if (topologyGraph.getTarget(edge) == *targetTopologyVertex) {
                    const auto connector = topologyGraph.getEdgeLabel(edge);
                    if (connector->fromInput(targetIndex)) {
                        const auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(sourceDomain);
                        assert(*(connector -> fromInput(targetIndex)) < (emitter -> getEmitterCount()), "Emitter index out of bounds");
                        if (sourceType == NodeType::Device) {
                            return (connector -> fromOutput(sourceIndex).occupied()) && (*connector -> fromOutput(sourceIndex) == targetIndex);
                        }
                        return true;
                    }
                }
            }
            return false;
        }

        IteratorRange<PotentialEdgeIterator<true>> RoutingConstraint::validEdgesFrom(const Builder &graph, VertexHandle source) {
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

        IteratorRange<PotentialEdgeIterator<false>> RoutingConstraint::validEdgesTo(const Builder &graph, VertexHandle target) {
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
            Iterator& end, size_t index, size_t findex, const GraphBuilderBase<RoutingGraph>* g):
            currentConnector(itr), endConnector(end), currentIndex(index), fixedDomain(domain), fixedIndex(findex), graph(g) {
            assert(fixedDomain, "Fixed domain is null");
            if constexpr (Forward){
                trivialIterator = false;
                //If we're iterating forward from a device domain, there's only one receiver that a given emitter would
                //connect to
                if (!fixedDomain -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    trivialIterator = true;
                    //We also have to advance to the unique valid state
                    while (currentConnector != endConnector) {
                        auto edge = currentConnector.operator*();
                        const auto conn = topology::getTopologyGraph()->getEdgeLabel(edge);
                        //If the connector connects to the relevant emitter, set the relevant receiver index and break
                        if (conn -> fromOutput(findex)) {
                            currentIndex = *conn -> fromOutput(findex);
                            break;
                        }
                        //Otherwise go to the next connector
                        else {
                            currentConnector.operator++();
                        }
                    }
                }
            }
            else {
                advanceToValidState();
            }
            //If we're iterating backwards from a device domain, then it's already the case that itr == end
            //so iteration is already trivial
        }

        template<>
        PotentialEdgeIterator<true> &PotentialEdgeIterator<true>::operator++() {
            //If we're at the end, just return
            //The wonky syntax is because the compiler's type deduction is struggling to understand how to
            //make sense of the == operator, so we have to manually call .operator!= (as .operator== is unimplemented)
            //and invert the result
            if (!currentConnector.operator!=(endConnector)) {
                return *this;
            }
            //If the source is a device, we have a trivial iteration strategy - just go to the end
            if (trivialIterator) {
                currentConnector = endConnector;
                currentIndex = 0;
                return *this;
            }
            //This will only ever be called if we already know the topology graph is preconstructed from createRoutingGraphBuilder
            //so it is safe to dereference
            currentIndex++;
            const auto& topGraph = *topology::getTopologyGraph();
            const auto edge = currentConnector.operator*();
            const auto targetVertex = topGraph.getTarget(edge);
            const auto& targetDomain = topGraph.getVertexLabel(targetVertex);
            const auto receiverDomain = crocos_dynamic_cast<platform::InterruptReceiver>(targetDomain);
            assert(receiverDomain, "Target domain must be a receiver");
            if (currentIndex >= receiverDomain->getReceiverCount()) {
                currentIndex = 0;
                currentConnector.operator++();
            }
            return *this;
        }

        template<>
        PotentialEdgeIterator<false> &PotentialEdgeIterator<false>::operator++() {
            //If we're at the end, just return
            //The wonky syntax is because the compiler's type deduction is struggling to understand how to
            //make sense of the == operator, so we have to manually call .operator!= (as .operator== is unimplemented)
            //and invert the result
            if (!currentConnector.operator!=(endConnector)) {
                return *this;
            }
            //This will only ever be called if we already know the topology graph is preconstructed from createRoutingGraphBuilder
            //so it is safe to dereference
            currentIndex++;
            const auto& topGraph = *topology::getTopologyGraph();
            const auto edge = currentConnector.operator*();
            const auto sourceVertex = topGraph.getSource(edge);
            const auto& sourceDomain = topGraph.getVertexLabel(sourceVertex);
            const auto emitterDomain = crocos_dynamic_cast<platform::InterruptEmitter>(sourceDomain);
            assert(emitterDomain, "Source domain must be an emitter");
            if (currentIndex >= emitterDomain->getEmitterCount()) {
                currentIndex = 0;
                currentConnector.operator++();
            }
            advanceToValidState();
            return *this;
        }

        template<bool Forward>
        void PotentialEdgeIterator<Forward>::advanceToValidState() {
            if constexpr (!Forward) {
                while (currentConnector.operator!=(endConnector)) {
                    auto& topGraph = *topology::getTopologyGraph();
                    auto edge = currentConnector.operator*();
                    auto sourceVertex = topGraph.getSource(edge);
                    const auto& sourceDomain = topGraph.getVertexLabel(sourceVertex);
                    
                    // If source is a device, find the specific emitter index for fixedIndex
                    if (!sourceDomain->instanceof(TypeID_v<platform::InterruptReceiver>)) {
                        auto& connector = topGraph.getEdgeLabel(edge);
                        auto mappedOutput = connector->fromInput(fixedIndex);
                        
                        if (mappedOutput.occupied()) {
                            if (currentIndex > *mappedOutput) {
                                // We've passed the valid index, move to next connector
                                currentConnector.operator++();
                                currentIndex = 0;
                                continue;
                            } else {
                                // currentIndex <= *mappedOutput, jump to the valid index
                                currentIndex = *mappedOutput;
                                return; // We're now in a valid state
                            }
                        } else {
                            // No mapping for fixedIndex on this connector, try next
                            currentConnector.operator++();
                            currentIndex = 0;
                            continue;
                        }
                    } else {
                        // Controller case - currentIndex is already valid
                        return;
                    }
                }
                currentIndex = 0;
                // Reached end
            }
            // For Forward = true, this function does nothing
        }

        template<bool Forward>
        BuilderVertexHandle<RoutingGraph> PotentialEdgeIterator<Forward>::operator*() const {
            assert(this -> currentConnector != this -> endConnector, "Tried to dereference end connector");
            // Find the target domain and create a RoutingNodeLabel for it
            auto& topologyGraph = *topology::getTopologyGraph();
            auto edge = currentConnector.operator*();
            
            SharedPtr<platform::InterruptDomain> targetDomain;
            NodeType targetType;
            
            if constexpr (Forward) {
                auto targetVertex = topologyGraph.getTarget(edge);
                targetDomain = topologyGraph.getVertexLabel(targetVertex);
                targetType = NodeType::Input;
            } else {
                auto sourceVertex = topologyGraph.getSource(edge);
                targetDomain = topologyGraph.getVertexLabel(sourceVertex);
                // For backward iteration, we need to determine if source is a device or input
                if (targetDomain->instanceof(TypeID_v<platform::InterruptReceiver>)) {
                    targetType = NodeType::Input;
                } else {
                    targetType = NodeType::Device;
                }
            }
            
            const RoutingNodeLabel targetLabel(targetDomain, currentIndex, targetType);
            auto vertex = this->graph->getVertexByLabel(targetLabel);
            assert(vertex.occupied(), "Target vertex must exist in routing graph");
            return *vertex;
        }
    }
}

size_t DefaultHasher<kernel::hal::interrupts::managed::RoutingNodeLabel>::operator()(const kernel::hal::interrupts::managed::RoutingNodeLabel& label) const {
    return label.hash();
}

// Explicit template instantiations to force code generation
template class kernel::hal::interrupts::managed::PotentialEdgeIterator<true>;
template class kernel::hal::interrupts::managed::PotentialEdgeIterator<false>;