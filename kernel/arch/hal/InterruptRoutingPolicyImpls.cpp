//
// Created by Spencer Martin on 8/29/25.
//
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <kernel.h>

namespace kernel::hal::interrupts::managed {
    InterruptReceiverLoadComparator::InterruptReceiverLoadComparator(HashMap<RoutingNodeLabel, size_t>& loads) : receiverLoads(loads) {}
    // Returns true if a is "heavier" than b, so that the heap promotes the least-loaded.
    bool InterruptReceiverLoadComparator::operator()(const RoutingNodeLabel& a, const RoutingNodeLabel& b) const{
        return receiverLoads[a] > receiverLoads[b];
    }

    FreelyRoutableDomainGreedyRouter::FreelyRoutableDomainGreedyRouter(RoutingGraphBuilder &b, SharedPtr<platform::InterruptDomain> &d, DomainReceiverLoadMap& l) : builder(b), domain(d), loads(l),
    comparator(loads), heaps{DomainReceiverHeap(comparator), DomainReceiverHeap(comparator), DomainReceiverHeap(comparator)}{
        assert(domain -> instanceof(TypeID_v<platform::FreeRoutableDomain>), "Can't construct a FreelyRoutableDomainGreedyRouter with a domain that isn't freely routable");
        auto nodeLabel = RoutingNodeLabel(domain, 0);
        auto node = b.getVertexByLabel(nodeLabel);
        assert(node.occupied(), "Node not found");
        for (auto v : b.validEdgesFromIgnoringTriggerType(*node)) {
            auto targetLabel = b.getVertexLabel(v);
            auto targetTriggerType = b.getConnectedComponentTriggerType(v);
            heaps[static_cast<size_t>(targetTriggerType)].push(*targetLabel);
        }
    }

    bool FreelyRoutableDomainGreedyRouter::routeAll() {
        bool success = true;
        const auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
        const auto receiverCount = receiver -> getReceiverCount();
        for (size_t i = 0; i < receiverCount; i++) {
            auto didRouteSuccessfully = route(i).occupied();
            if (!didRouteSuccessfully) {
                klog << "FreelyRoutableDomainGreedyRouter::routeAll() failed on domain of type " <<
                    receiver -> type_name() << " at receiver index " << i << "\n";
                success = false;
            }
        }
        return success;
    }

    Optional<RoutingGraphBuilder::EdgeHandle> FreelyRoutableDomainGreedyRouter::route(size_t receiverIndex) {
        auto sourceLabel = RoutingNodeLabel(domain, receiverIndex);
        auto sourceTriggerType = builder.getConnectedComponentTriggerType(*builder.getVertexByLabel(sourceLabel));
        Optional<RoutingNodeLabel> bestCandidate;
        Optional<RoutingNodeTriggerType> bestTriggerType;
        auto consider = [&](RoutingNodeTriggerType type) {
            if (!heaps[static_cast<size_t>(type)].empty()) {
                auto cand = heaps[static_cast<size_t>(type)].top();
                if (!bestCandidate.occupied() || comparator(*bestCandidate, cand)) {
                    bestCandidate = cand;
                    bestTriggerType = type;
                }
            }
        };
        consider(RoutingNodeTriggerType::TRIGGER_UNDETERMINED);
        consider(sourceTriggerType == RoutingNodeTriggerType::TRIGGER_LEVEL ? RoutingNodeTriggerType::TRIGGER_LEVEL : RoutingNodeTriggerType::TRIGGER_EDGE);
        if (!bestCandidate.occupied()) {
            return {};
        }
        auto target = builder.getVertexByLabel(*bestCandidate);
        auto out = builder.addEdge(*builder.getVertexByLabel(sourceLabel), *target);
        heaps[static_cast<size_t>(*bestTriggerType)].pop();
        loads[*bestCandidate] += loads[sourceLabel];
        if (*bestTriggerType == RoutingNodeTriggerType::TRIGGER_UNDETERMINED) {
            heaps[static_cast<size_t>(sourceTriggerType)].push(*bestCandidate);
        }
        else {
            heaps[static_cast<size_t>(*bestTriggerType)].push(*bestCandidate);
        }
        return out;
    }

    RoutingGraph GreedyRoutingPolicy::buildRoutingGraph(RoutingGraphBuilder& builder) {
        const auto topologyGraph = *topology::getTopologyGraph();
        const auto topologyDomains = algorithm::graph::topologicalSort(topologyGraph);
        HashMap<SharedPtr<platform::InterruptDomain>, size_t> domainOrder;
        for (size_t i = 0; i < topologyDomains.size(); i++) {
            const auto domain = topologyGraph.getVertexLabel(topologyDomains[i]);
            domainOrder.insert(domain, i);
        }

        DomainReceiverLoadMap receiverLoads;

        auto preexistingEdges = Vector<RoutingGraphBuilder::EdgeHandle>(builder.currentEdges());
        preexistingEdges.sort([&](const RoutingGraphBuilder::EdgeHandle& a, const RoutingGraphBuilder::EdgeHandle& b) {
            auto la = builder.getVertexLabel(builder.getEdgeSource(a));
            auto lb = builder.getVertexLabel(builder.getEdgeSource(b));
            const auto domainA = la -> domain();
            const auto domainB = lb -> domain();
            if (domainA == domainB) return la -> index() < lb -> index();
            return domainOrder[domainA] > domainOrder[domainB];
        });

        for (auto vertex : topologyDomains) {
            auto domain = topologyGraph.getVertexLabel(vertex);
            //If the domain is a pure emitter, initialize all loads to 1
            if (!domain -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                assert(domain -> instanceof(TypeID_v<platform::InterruptEmitter>), "Interrupt domain must be at least receiver or emitter");
                auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                for (size_t i = 0; i < emitter -> getEmitterCount(); i++) {
                    auto label = RoutingNodeLabel(domain, i);
                    receiverLoads.insert(label, 1);
                }
            }
            //If there are preexisting edges coming from this domain, propagate the loads forwards
            while (preexistingEdges.size() > 0 && builder.getVertexLabel(builder.getEdgeSource(*preexistingEdges.top())) -> domain() == domain) {
                auto sourceLabel = builder.getVertexLabel(builder.getEdgeSource(*preexistingEdges.top()));
                auto targetLabel = builder.getVertexLabel(builder.getEdgeTarget(*preexistingEdges.top()));
                auto sourceLoad = receiverLoads[*sourceLabel];
                //if receiverLoads[*targetLabel] does not exist, it is automatically initialized to 0
                receiverLoads[*targetLabel] += sourceLoad;
                preexistingEdges.pop();
            }
            //If 'domain' is freely routable, then all interrupt receivers can be mapped
            //to any possible output, so we can just keep one global list of possible receivers to map to
            if (domain -> instanceof(TypeID_v<platform::FreeRoutableDomain>)) {
                auto greedyFreeRouter = FreelyRoutableDomainGreedyRouter(builder, domain, receiverLoads);
                assert(greedyFreeRouter.routeAll(), "Failed to route all receivers for freely routable domain");
            }
            //Otherwise, we have no hope of doing anything clever - we've gotta brute force it :(
            else if (domain -> instanceof(TypeID_v<platform::RoutableDomain>)) {
                auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
                for (size_t i = 0; i < receiver -> getReceiverCount(); i++) {
                    auto label = RoutingNodeLabel(domain, i);
                    //Skip routing receivers that aren't eventually connected to any devices.
                    if (receiverLoads[label] == 0) continue;
                    auto sourceNode = *builder.getVertexByLabel(label);
                    Optional<RoutingGraphBuilder::VertexHandle> bestCandidate;
                    size_t bestLoad = 0;
                    for (auto dest : builder.getValidEdgesFrom(sourceNode)) {
                        if (!bestCandidate.occupied()) {
                            bestCandidate = dest;
                            bestLoad = receiverLoads[*builder.getVertexLabel(dest)];
                        }
                        else {
                            auto destLoad = receiverLoads[*builder.getVertexLabel(dest)];
                            if (destLoad < bestLoad) {
                                bestCandidate = dest;
                                bestLoad = destLoad;
                            }
                        }
                    }
                    if (!bestCandidate.occupied()) {
                        klog << "GreedyRoutingPolicy::buildRoutingGraph() failed to find a valid destination for receiver " << receiver -> type_name() << " at index " << i << "\n";
                    }
                    assert(bestCandidate.occupied(), "No valid destination for interrupt receiver");
                    builder.addEdge(sourceNode, *bestCandidate);
                    receiverLoads[*builder.getVertexLabel(*bestCandidate)] += receiverLoads[label];
                }
            }
        }
        return *(builder.build());
    }
}