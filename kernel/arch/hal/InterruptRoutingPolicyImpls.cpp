//
// Created by Spencer Martin on 8/29/25.
//
#include <arch/hal/interrupts.h>
#include <core/algo/GraphAlgorithms.h>
#include <kernel.h>

namespace kernel::hal::interrupts::managed {
    struct InterruptReceiverLoadComparator {
        HashMap<RoutingNodeLabel, size_t>* receiverLoads;
        InterruptReceiverLoadComparator(HashMap<RoutingNodeLabel, size_t>& loads) : receiverLoads(&loads) {}
        bool operator()(const RoutingNodeLabel& a, const RoutingNodeLabel& b) {
            return (*receiverLoads)[a] > (*receiverLoads)[b];
        }
    };

    using DomainReceiverHeap = Heap<RoutingNodeLabel, InterruptReceiverLoadComparator>;
    using DomainReceiverLoadMap = VertexAnnotation<DomainReceiverHeap, topology::TopologyGraph>;

    RoutingGraph GreedyRoutingPolicy::buildRoutingGraph(RoutingGraphBuilder& builder) {
        const auto topologyGraph = *topology::getTopologyGraph();
        const auto topologyDomains = algorithm::graph::topologicalSort(topologyGraph);
        HashMap<SharedPtr<platform::InterruptDomain>, size_t> domainOrder;
        for (size_t i = 0; i < topologyDomains.getSize(); i++) {
            const auto domain = topologyGraph.getVertexLabel(topologyDomains[i]);
            domainOrder.insert(domain, i);
        }
        HashMap<RoutingNodeLabel, size_t> receiverLoads;
        const InterruptReceiverLoadComparator comparator(receiverLoads);

        auto preexistingEdges = Vector<RoutingGraphBuilder::EdgeHandle>(builder.currentEdges());
        preexistingEdges.sort([&](const RoutingGraphBuilder::EdgeHandle& a, const RoutingGraphBuilder::EdgeHandle& b) {
            const auto domainA = builder.getVertexLabel(builder.getEdgeSource(a)) -> domain();
            const auto domainB = builder.getVertexLabel(builder.getEdgeSource(b)) -> domain();
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
            while (preexistingEdges.getSize() > 0 && builder.getVertexLabel(builder.getEdgeSource(*preexistingEdges.top())) -> domain() == domain) {
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
                Heap<RoutingNodeLabel, InterruptReceiverLoadComparator> bestReceivers(comparator);
                auto firstReceiver = builder.getVertexByLabel(RoutingNodeLabel(domain, 0));
                assert(firstReceiver, "Routable domain must have at least one receiver");
                for (auto dest : builder.getValidEdgesFrom(*firstReceiver)) {
                    auto label = builder.getVertexLabel(dest);
                    assert(label, "Routing vertex should be labeled");
                    bestReceivers.push(*label);
                }
                auto receiver = crocos_dynamic_cast<platform::InterruptReceiver>(domain);
                for (size_t i = 0; i < receiver -> getReceiverCount(); i++) {
                    auto label = RoutingNodeLabel(domain, i);
                    //Skip routing receivers that aren't eventually connected to any devices.
                    if (receiverLoads[label] == 0) continue;
                    auto target = bestReceivers.pop();
                    receiverLoads[target] += receiverLoads[label];
                    bestReceivers.push(target);
                    auto edge = builder.addEdge(*builder.getVertexByLabel(label), *builder.getVertexByLabel(target));
                    assert(edge.occupied(), "Edge should be created");
                }
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
                    assert(bestCandidate.occupied(), "No valid destination for interrupt receiver");
                    builder.addEdge(sourceNode, *bestCandidate);
                    receiverLoads[*builder.getVertexLabel(*bestCandidate)] += receiverLoads[label];
                }
            }
        }
        return *(builder.build());
    };
}
