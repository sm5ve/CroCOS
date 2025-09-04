//
// Created by Spencer Martin on 8/29/25.
//
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <core/ds/LinkedList.h>
#include <liballoc/InternalAllocator.h>
#include <liballoc/SlabAllocator.h>

namespace kernel::hal::interrupts::managed {
    struct EOIChain {
        Vector<SharedPtr<platform::InterruptDomain>> reverseSortedDomains;
        bool operator==(const EOIChain & other) const {
            if (other.reverseSortedDomains.getSize() != reverseSortedDomains.getSize())
                return false;
            for (size_t i = 0; i < reverseSortedDomains.getSize(); i++) {
                if (reverseSortedDomains[i] != other.reverseSortedDomains[i])
                    return false;
            }
            return true;
        }

        void addDomain(const SharedPtr<platform::InterruptDomain>& domain, HashMap<SharedPtr<platform::InterruptDomain>, size_t>& domainOrder) {
            reverseSortedDomains.mergeIn(domain, [&](const SharedPtr<platform::InterruptDomain>& a, const SharedPtr<platform::InterruptDomain>& b) {
                return domainOrder[a] > domainOrder[b];
            });
        }
    };
}

template<>
struct DefaultHasher<kernel::hal::interrupts::managed::EOIChain> {
    size_t operator()(const kernel::hal::interrupts::managed::EOIChain& chain) const {
        size_t hash = 0xcbf29ce484222325ULL; // FNV offset basis for 64-bit

        for (auto& ptr : chain.reverseSortedDomains) {
            constexpr size_t fnvPrime = 0x100000001b3ULL;
            const size_t ptrVal = reinterpret_cast<size_t>(ptr.get());
            hash ^= ptrVal;
            hash *= fnvPrime;
        }

        return hash;
    }
};

namespace kernel::hal::interrupts::managed {
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
    using InterruptHandlerListForVector = Vector<InterruptHandlerPointerRef>;
    using SourceToHandlerMap = HashMap<InterruptSourceHandle, InterruptHandlerPointerRef>;
    WITH_GLOBAL_CONSTRUCTOR(SourceToHandlerMap, registeredHandlers);
    UniquePtr<InterruptHandlerListForVector> handlersByVector[256];

    void populateHandlerTable(const RoutingGraph& routingGraph, VertexAnnotation<Optional<size_t>, RoutingGraph>& annotation) {
        for (auto& ptr : handlersByVector) {
            ptr.reset();
        }

        for (const auto v : routingGraph.vertices()) {
            const auto& label = routingGraph.getVertexLabel(v);
            if (!label.domain() -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                const auto vectorNumber = *annotation[v];
                if (!handlersByVector[vectorNumber]) {
                    handlersByVector[vectorNumber] = make_unique<InterruptHandlerListForVector>();
                }
                handlersByVector[vectorNumber] -> push(registeredHandlers[label]);
            }
        }

        for (auto& ptr : handlersByVector) {
            if (ptr) ptr -> shrinkToFit();
        }
    }

    VertexAnnotation<Optional<size_t>, RoutingGraph> computeFinalVectorNumbers(const RoutingGraph& routingGraph) {
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
            const auto& l1 = routingGraph.getVertexLabel(t1);
            const auto& l2 = routingGraph.getVertexLabel(t2);
            return domainOrder[l1.domain()] > domainOrder[l2.domain()];
        });

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
                const size_t vectorNumber = *finalVectorNumber[source];
                if (!handlersByVector[vectorNumber]) {
                    handlersByVector[vectorNumber] = make_unique<InterruptHandlerListForVector>();
                }
                handlersByVector[vectorNumber] -> push(registeredHandlers[sourceLabel]);
            }
        }

        return finalVectorNumber;
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
                        if (routingGraph.outDegree(*routingVertex) == 0) {
                            maskable -> setReceiverMask(i, true);
                        } else {
                            maskable -> setReceiverMask(i, false);
                        }
                    }
                }
            }
        }
    }

    size_t countEOIDomains() {
        const auto& topologyGraph = *topology::getTopologyGraph();
        size_t count = 0;
        for (const auto v : topologyGraph.vertices()) {
            if (topologyGraph.getVertexLabel(v) -> instanceof(TypeID_v<platform::EOIDomain>)) {
                ++count;
            }
        }
        return count;
    }

    void updateRouting() {
        auto& policy = getRoutingPolicy();
        const auto routingGraph = policy.buildRoutingGraph(*createRoutingGraphBuilder());
        configureRoutableDomains(routingGraph);
        auto finalVectorNumbers = computeFinalVectorNumbers(routingGraph);
        populateHandlerTable(routingGraph, finalVectorNumbers);
        disableUnmappedInterrupts(routingGraph);
    }

    void dispatchInterrupt(InterruptFrame& frame) {
        if (handlersByVector[frame.vector_index].get() != nullptr) {
            for (auto& handler : *handlersByVector[frame.vector_index]) {
                //It is possible that we have some uninitialized handlers for emitters routed to this vector, hence
                //we must check for null
                if (*handler)
                    (**handler)(frame);
            }
        }
    }
}
