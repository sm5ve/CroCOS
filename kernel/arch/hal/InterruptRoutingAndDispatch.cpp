//
// Created by Spencer Martin on 8/29/25.
//
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <core/ds/LinkedList.h>

namespace kernel::hal::interrupts::managed{
    UniquePtr<InterruptRoutingPolicy> currentRoutingPolicy;

    UniquePtr<InterruptRoutingPolicy> createDefaultRoutingPolicy(){
        return UniquePtr<InterruptRoutingPolicy>(new GreedyRoutingPolicy());
    }

    InterruptRoutingPolicy& getRoutingPolicy(){
        if(!currentRoutingPolicy){
            currentRoutingPolicy = createDefaultRoutingPolicy();
        }
        return *currentRoutingPolicy;
    }

    void configureRoutableDomains(const RoutingGraph& routingGraph) {
        for (const auto edge : routingGraph.edges()) {
            const auto& sourceLabel = routingGraph.getVertexLabel(routingGraph.getSource(edge));
            const auto& targetLabel = routingGraph.getVertexLabel(routingGraph.getTarget(edge));
            auto sourceDomain = sourceLabel.domain();
            auto targetDomain = targetLabel.domain();

            if (const auto routable = crocos_dynamic_cast<platform::RoutableDomain>(sourceDomain)) {
                auto& topologyGraph = *topology::getTopologyGraph();
                auto sourceTopVertex = topologyGraph.getVertexByLabel(sourceDomain);
                auto targetTopVertex = topologyGraph.getVertexByLabel(targetDomain);
                auto topEdge = topologyGraph.findEdge(*sourceTopVertex, *targetTopVertex);
                //not horrifically efficient to keep looking this up, but
                //most domains should only have one connector coming out of them
                //and so this should basically be O(1) in practice
                const auto connector = topologyGraph.getEdgeLabel(*topEdge);

                auto routedEmitterIndex = connector -> fromInput(targetLabel.index());
                assert(routedEmitterIndex, "Emitter index must be valid");
                routable -> routeInterrupt(sourceLabel.index(), *routedEmitterIndex);
            }
        }
    }

    using InterruptHandlerPointerRef = SharedPtr<UniquePtr<InterruptHandler>>;
    using InterruptHandlerListForVector = LinkedList<InterruptHandlerPointerRef>;
    using SourceToHandlerMap = HashMap<InterruptSourceHandle, InterruptHandlerPointerRef>;
    WITH_GLOBAL_CONSTRUCTOR(SourceToHandlerMap, registeredHandlers);
    UniquePtr<InterruptHandlerListForVector> handlersByVector[256];

    void populateHandlerTable(const RoutingGraph& routingGraph) {
        for (auto& ptr : handlersByVector) {
            ptr.reset();
        }
        const auto& topologyGraph = *topology::getTopologyGraph();
        auto topologyNodes = algorithm::graph::topologicalSort(topologyGraph);
        HashMap<SharedPtr<platform::InterruptDomain>, size_t> domainOrder;
        for (size_t i = 0; i < topologyNodes.getSize(); i++) {
            const auto domain = topologyGraph.getVertexLabel(topologyNodes[i]);
            domainOrder.insert(domain, i);
        }
        using Edge = RoutingGraph::Edge;
        Vector<Edge> edgeList;
        for (const auto edge : routingGraph.edges()) {edgeList.push(edge);}
        edgeList.sort([&](const Edge& a, const Edge& b) {
            const auto t1 = routingGraph.getTarget(a);
            const auto t2 = routingGraph.getTarget(b);
            const auto l1 = routingGraph.getVertexLabel(t1);
            const auto l2 = routingGraph.getVertexLabel(t2);
            return domainOrder[l1.domain()] > domainOrder[l2.domain()];
        });
        /*for (const auto edge : edgeList) {
            const auto target = routingGraph.getTarget(edge);
            const auto source = routingGraph.getSource(edge);
            const auto& targetLabel = routingGraph.getVertexLabel(target);
            const auto& sourceLabel = routingGraph.getVertexLabel(source);
            klog << "connection from " << sourceLabel.domain()->type_name() << " pin " << sourceLabel.index() << " to " << targetLabel.domain()->type_name() << " pin " << targetLabel.index() << "\n";
        }*/
        VertexAnnotation<Optional<size_t>, RoutingGraph> finalVectorNumber(routingGraph);
        for (const auto edge : edgeList) {
            const auto target = routingGraph.getTarget(edge);
            const auto source = routingGraph.getSource(edge);
            const auto& targetLabel = routingGraph.getVertexLabel(target);
            const auto& sourceLabel = routingGraph.getVertexLabel(source);
            if (targetLabel.domain() -> instanceof(TypeID_v<platform::CPUInterruptVectorFile>)) {
                finalVectorNumber[target] = targetLabel.index();
            }
            finalVectorNumber[source] = finalVectorNumber[target];
            if (!sourceLabel.domain() -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                if (!registeredHandlers.contains(sourceLabel)) {
                    registeredHandlers.insert(sourceLabel, make_shared<UniquePtr<InterruptHandler>>(nullptr));
                }
                size_t vectorNumber = *finalVectorNumber[source];
                if (!handlersByVector[vectorNumber]) {
                    handlersByVector[vectorNumber] = make_unique<InterruptHandlerListForVector>();
                }
                handlersByVector[vectorNumber] -> pushBack(registeredHandlers[sourceLabel]);
            }
        }
    }

    void registerHandler(const InterruptSourceHandle& interruptSource, UniquePtr<InterruptHandler>&& handler) {
        if (registeredHandlers.contains(interruptSource)) {
            *registeredHandlers[interruptSource] = move(handler);
        }
        else {
            registeredHandlers.insert(interruptSource, make_shared<UniquePtr<InterruptHandler>>(move(handler)));
        }
    }

    void disableUnmappedInterrupts(const RoutingGraph& routingGraph) {
        const auto& topologyGraph = *topology::getTopologyGraph();
        for (const auto v : topologyGraph.vertices()) {
            const auto& domain = topologyGraph.getVertexLabel(v);
            if (const auto maskable = crocos_dynamic_cast<platform::MaskableDomain>(domain)) {
                if (const auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain)) {
                    for (size_t i = 0; i < receiver -> getReceiverCount(); i++) {
                        const auto label = RoutingNodeLabel(domain, i);
                        const auto routingVertex = routingGraph.getVertexByLabel(label);
                        if (routingGraph.outDegree(*routingVertex) != 0) {
                            maskable -> setReceiverMask(i, true);
                        }
                    }
                }
            }
        }
    }

    void updateRouting() {
        auto& policy = getRoutingPolicy();
        const auto routingGraph = policy.buildRoutingGraph(*createRoutingGraphBuilder());
        configureRoutableDomains(routingGraph);
        populateHandlerTable(routingGraph);
        disableUnmappedInterrupts(routingGraph);
    }

    void dispatchInterrupt(hal::InterruptFrame& frame) {
        if (!handlersByVector[frame.vector_index]->empty()) {
            for (auto& handler : *handlersByVector[frame.vector_index]) {
                (**handler)(frame);
            }
        }
    }
}
