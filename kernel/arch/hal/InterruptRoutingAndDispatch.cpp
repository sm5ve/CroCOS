//
// Created by Spencer Martin on 8/29/25.
//
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <core/ds/LinkedList.h>
#include <liballoc/InternalAllocator.h>
#include <liballoc/SlabAllocator.h>
#include <arch/hal/hal.h>

namespace kernel::hal::interrupts::managed {
    struct EOIChain {
        Vector<SharedPtr<platform::EOIDomain>> sortedDomains;
        bool operator==(const EOIChain & other) const {
            if (other.sortedDomains.size() != sortedDomains.size())
                return false;
            for (size_t i = 0; i < sortedDomains.size(); i++) {
                if (sortedDomains[i] != other.sortedDomains[i])
                    return false;
            }
            return true;
        }

        EOIChain() {}
        EOIChain(EOIChain&& other) : sortedDomains(move(other.sortedDomains)) {}
        EOIChain(EOIChain const& other) : sortedDomains(other.sortedDomains) {}
        EOIChain(Vector<SharedPtr<platform::EOIDomain>>&& domains) : sortedDomains(move(domains)) {}
    };
}

template<>
struct DefaultHasher<kernel::hal::interrupts::managed::EOIChain> {
    size_t operator()(const kernel::hal::interrupts::managed::EOIChain& chain) const {
        size_t hash = 0xcbf29ce484222325ULL; // FNV offset basis for 64-bit

        for (auto& ptr : chain.sortedDomains) {
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
    ARRAY_WITH_GLOBAL_CONSTRUCTOR(UniquePtr<InterruptHandlerListForVector>, CPU_INTERRUPT_COUNT, handlersByVector);

    void populateHandlerTable(const RoutingGraph& routingGraph, VertexAnnotation<Optional<size_t>, RoutingGraph>& annotation) {
        for (auto& ptr : handlersByVector) {
            ptr.reset();
        }

        for (const auto v : routingGraph.vertices()) {
            const auto& label = routingGraph.getVertexLabel(v);
            if (!label.domain() -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                const auto vectorNumber = *annotation[v];
                //klog << "Mapping " << label.domain() -> type_name() << " emitter " << label.index() << " -> " << vectorNumber << "\n";
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
        auto& domainOrder = topology::topologicalOrderMap();
        using Edge = RoutingGraph::Edge;
        Vector<Edge> edgeList;
        for (const auto edge : routingGraph.edges()) {edgeList.push(edge);}
        edgeList.sort([&](const Edge& a, const Edge& b) {
            const auto t1 = routingGraph.getTarget(a);
            const auto t2 = routingGraph.getTarget(b);
            const auto& l1 = routingGraph.getVertexLabel(t1);
            const auto& l2 = routingGraph.getVertexLabel(t2);
            if (l1.domain() == l2.domain()) {return l1.index() < l2.index();}
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

    void enableOnlyMappedInterrupts(const RoutingGraph& routingGraph) {
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

    Vector<Tuple<RoutingGraph::Vertex, size_t>> getSourcesByResultingVector(VertexAnnotation<Optional<size_t>, RoutingGraph>& vectorNumberMap, const RoutingGraph& routingGraph) {
        Vector<Tuple<RoutingGraph::Vertex, size_t>> out;
        for (auto v : routingGraph.vertices()) {
            auto& label = routingGraph.getVertexLabel(v);
            //Only iterate over pure emitters
            if (!label.domain() -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                auto destination = vectorNumberMap[v];
                if (destination.occupied()) {
                    out.push({v, *destination});
                }
                else {
                    klog << "Warning: " << label.domain() -> type_name() << " emitter number " << label.index() << " was not routed to an interrupt vector\n";
                }
            }
        }
        auto& topOrder = topology::topologicalOrderMap();
        out.sort([&](auto& a, auto& b) {
            if (a.second() == b.second()) {
                auto la = routingGraph.getVertexLabel(a.first());
                auto lb = routingGraph.getVertexLabel(b.first());
                if (la.domain() == lb.domain()) {
                    return la.index() < lb.index();
                }
                return topOrder[la.domain()] < topOrder[lb.domain()];
            }
            return a.second() < b.second();
        });
        return out;
    }

    EOIChain buildChainForVector(Vector<Tuple<RoutingGraph::Vertex, size_t>>& sortedInterruptSources, const RoutingGraph& routingGraph, const size_t targetVector, const size_t maxEOIDeviceCount) {
        HashSet<SharedPtr<platform::EOIDomain>> eoiDomains;
        while (!sortedInterruptSources.empty() && eoiDomains.size() != maxEOIDeviceCount && sortedInterruptSources.top()->second() == targetVector) {
            auto vertex = sortedInterruptSources.top()->first();
            while (true) {
                auto domain = routingGraph.getVertexLabel(vertex).domain();
                if (auto eoidomain = crocos_dynamic_cast<platform::EOIDomain>(domain)) {
                    eoiDomains.insert(eoidomain); //automatically discards duplicates
                }
                bool found = false;
                //Hack to get around the fact that we can't arbitrarily index into edge lists.
                //I should probably add that feature at some point...
                //In practice, this list is at most 1 element long.
                for (auto e : routingGraph.outgoingEdges(vertex)) {
                    found = true;
                    vertex = routingGraph.getTarget(e);
                    break;
                }
                if (!found) {break;}
            }
            sortedInterruptSources.pop();
        }
        (void)routingGraph;
        if (eoiDomains.size() == 0) {
            return {};
        }
        Vector<SharedPtr<platform::EOIDomain>> domains;
        for (auto& e : eoiDomains) {
            domains.push(e);
        }
        auto& topologicalOrder = topology::topologicalOrderMap();
        //Sort the domains ahead of time - this makes issuing EOIs even simpler, and also
        //puts the list in a canonical order so we can compare against existing EOIChains.
        domains.sort([&topologicalOrder](auto& a, auto& b) {
            //ugly that I have to dynamic cast here, but it's either this or I dynamic cast in the interrupt
            //handler
            const auto ageneric = crocos_dynamic_cast<platform::InterruptDomain>(a);
            const auto bgeneric = crocos_dynamic_cast<platform::InterruptDomain>(b);
            return topologicalOrder[ageneric] < topologicalOrder[bgeneric];
        });
        return {move(domains)};
    }

    struct EOIBehaviorMetadata {
        RoutingNodeTriggerType triggerType;
        SharedPtr<EOIChain> chain = SharedPtr<EOIChain>::null();

        EOIBehaviorMetadata() : triggerType(TRIGGER_UNDETERMINED) {}
    };

    ARRAY_WITH_GLOBAL_CONSTRUCTOR(EOIBehaviorMetadata, CPU_INTERRUPT_COUNT, eoiBehaviorTable);

    void populateEOIBehaviorTable(const RoutingGraph& routingGraph, VertexAnnotation<Optional<size_t>, RoutingGraph>& vectorNumberMap) {
        auto orderedSources = getSourcesByResultingVector(vectorNumberMap, routingGraph);
        auto eoiDeviceLimit = countEOIDomains();
        HashMap<EOIChain, size_t> eoiChains;
        auto indices = new size_t[CPU_INTERRUPT_COUNT];
        for (int i = CPU_INTERRUPT_COUNT - 1; i >= 0; --i) {
            const auto vectorNumber = static_cast<size_t>(i);
            auto eoiChain = buildChainForVector(orderedSources, routingGraph, vectorNumber, eoiDeviceLimit);
            if (!eoiChains.contains(eoiChain)) {
                eoiChains.insert(eoiChain, eoiChains.size());
            }
            indices[vectorNumber] = eoiChains.at(eoiChain);
        }
        auto eoiChainArray = new SharedPtr<EOIChain>[eoiChains.size()];
        for (auto pair : eoiChains.entries()) {
            eoiChainArray[pair.second()] = make_shared<EOIChain>(move(pair.first()));
        }
        for (size_t i = 0; i < CPU_INTERRUPT_COUNT; ++i) {
            eoiBehaviorTable[i].chain = eoiChainArray[indices[i]];
            auto vlabel = RoutingNodeLabel(platform::getCPUInterruptVectors(), i);
            auto vertex = routingGraph.getVertexByLabel(vlabel);
            assert(vertex.occupied(), "Vertex does not exist");
            eoiBehaviorTable[i].triggerType = routingGraph.getVertexColor(*vertex).triggerType;
        }
        klog << "Number of EOI chains: " << eoiChains.size() << "\n";
        delete[] eoiChainArray;
        delete[] indices;
    }

    void updateRouting() {
        InterruptDisabler disabler;
        auto& policy = getRoutingPolicy();
        const auto routingGraph = policy.buildRoutingGraph(*createRoutingGraphBuilder());
        configureRoutableDomains(routingGraph);
        auto finalVectorNumbers = computeFinalVectorNumbers(routingGraph);
        populateHandlerTable(routingGraph, finalVectorNumbers);
        enableOnlyMappedInterrupts(routingGraph);
        populateEOIBehaviorTable(routingGraph, finalVectorNumbers);
        topology::releaseCachedTopologicalOrdering();
    }

    void dispatchInterrupt(InterruptFrame& frame) {
        auto& eoiBehavior = eoiBehaviorTable[frame.vector_index];
        if (eoiBehavior.triggerType == TRIGGER_EDGE || eoiBehavior.triggerType == TRIGGER_UNDETERMINED) {
            auto& eoiChain = *(eoiBehavior.chain);
            for (auto d : eoiChain.sortedDomains) {
                d -> issueEOI(frame);
            }
        }
        else {
            assertUnimplemented("I don't yet have support for level-triggered interrupt EOIs");
        }
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
