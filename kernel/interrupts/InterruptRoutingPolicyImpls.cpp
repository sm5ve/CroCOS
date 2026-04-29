//
// Created by Spencer Martin on 8/29/25.
//
#include <interrupts/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <kernel.h>

namespace kernel::interrupts::managed {
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
                klog() << "FreelyRoutableDomainGreedyRouter::routeAll() failed on domain of type " <<
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

    // Single topological-order pass over all interrupt domains. For each domain in order
    // (sources first), the pass does three things:
    //
    //   Phase A — Load initialization: pure emitters start with load 1 per output, representing
    //             one interrupt source that needs a path to the CPU.
    //
    //   Phase B — Load propagation for pre-wired edges: fixed-routing and pure-device edges
    //             were pre-populated by createRoutingGraphBuilder(). They are sorted into
    //             `preexistingEdges` (a stack ordered by domain topological position, closest
    //             to sources on top) so that each domain's fixed edges are drained exactly when
    //             that domain is processed. Load from the source accumulates on the target,
    //             giving downstream domains an accurate view of how many interrupts they carry.
    //
    //   Phase C — Greedy routing: once loads are known, freely routable domains use
    //             FreelyRoutableDomainGreedyRouter (trigger-type-aware min-heap per trigger class).
    //             Non-free RoutableDomains use a simple linear scan for the least-loaded target.
    //             Note: the non-free path does not yet apply trigger-type-aware heap selection;
    //             it should eventually use the same strategy as the free path.
    RoutingGraph GreedyRoutingPolicy::buildRoutingGraph(RoutingGraphBuilder& builder) {
        const auto topologyGraph = *topology::getTopologyGraph();
        const auto topologyDomains = algorithm::graph::topologicalSort(topologyGraph);
        HashMap<SharedPtr<platform::InterruptDomain>, size_t> domainOrder;
        for (size_t i = 0; i < topologyDomains.size(); i++) {
            const auto domain = topologyGraph.getVertexLabel(topologyDomains[i]);
            domainOrder.insert(domain, i);
        }

        DomainReceiverLoadMap receiverLoads;

        // Pre-sort fixed/device edges so Phase B can drain them in topological order.
        // Sorted descending by domain position so `top()` always yields the edge whose
        // source domain comes earliest in topological order (processed next).
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

            // Phase A: initialize load for pure emitters (interrupt sources).
            if (!domain -> instanceof(TypeID_v<platform::InterruptReceiver>)) {
                assert(domain -> instanceof(TypeID_v<platform::InterruptEmitter>), "Interrupt domain must be at least receiver or emitter");
                auto emitter = crocos_dynamic_cast<platform::InterruptEmitter>(domain);
                for (size_t i = 0; i < emitter -> getEmitterCount(); i++) {
                    receiverLoads.insert(RoutingNodeLabel(domain, i), 1);
                }
            }

            // Phase B: propagate loads through pre-wired edges from this domain.
            while (preexistingEdges.size() > 0 && builder.getVertexLabel(builder.getEdgeSource(*preexistingEdges.top())) -> domain() == domain) {
                auto sourceLabel = builder.getVertexLabel(builder.getEdgeSource(*preexistingEdges.top()));
                auto targetLabel = builder.getVertexLabel(builder.getEdgeTarget(*preexistingEdges.top()));
                receiverLoads[*targetLabel] += receiverLoads[*sourceLabel];
                preexistingEdges.pop();
            }

            // Phase C: route freely routable domains using the trigger-type-aware greedy router,
            // and non-free routable domains using a simple least-loaded scan.
            if (domain -> instanceof(TypeID_v<platform::FreeRoutableDomain>)) {
                auto greedyFreeRouter = FreelyRoutableDomainGreedyRouter(builder, domain, receiverLoads);
                assert(greedyFreeRouter.routeAll(), "Failed to route all receivers for freely routable domain");
            }
            else if (domain -> instanceof(TypeID_v<platform::RoutableDomain>)) {
                auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
                for (size_t i = 0; i < receiver -> getReceiverCount(); i++) {
                    auto label = RoutingNodeLabel(domain, i);
                    if (receiverLoads[label] == 0) continue; // not connected to any source
                    auto sourceNode = *builder.getVertexByLabel(label);
                    Optional<RoutingGraphBuilder::VertexHandle> bestCandidate;
                    size_t bestLoad = 0;
                    for (auto dest : builder.getValidEdgesFrom(sourceNode)) {
                        auto destLoad = receiverLoads[*builder.getVertexLabel(dest)];
                        if (!bestCandidate.occupied() || destLoad < bestLoad) {
                            bestCandidate = dest;
                            bestLoad = destLoad;
                        }
                    }
                    if (!bestCandidate.occupied()) {
                        klog() << "GreedyRoutingPolicy::buildRoutingGraph() failed to find a valid destination for receiver " << receiver -> type_name() << " at index " << i << "\n";
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