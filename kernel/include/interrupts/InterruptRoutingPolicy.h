//
// Created by Spencer Martin on 8/29/25.
//

#ifndef CROCOS_INTERRUPTROUTINGPOLICY_H
#define CROCOS_INTERRUPTROUTINGPOLICY_H

#include <interrupts/InterruptGraphs.h>
#include <core/ds/Heap.h>

namespace kernel::interrupts::managed{
    using DomainReceiverLoadMap = HashMap<RoutingNodeLabel, size_t>;

    struct InterruptReceiverLoadComparator {
        DomainReceiverLoadMap& receiverLoads;
        explicit InterruptReceiverLoadComparator(HashMap<RoutingNodeLabel, size_t>& loads);
        bool operator()(const RoutingNodeLabel& a, const RoutingNodeLabel& b) const;
    };

    using DomainReceiverHeap = Heap<RoutingNodeLabel, InterruptReceiverLoadComparator>;

    class FreelyRoutableDomainGreedyRouter {
        RoutingGraphBuilder& builder;
        SharedPtr<platform::InterruptDomain> domain;
        DomainReceiverLoadMap& loads;
        InterruptReceiverLoadComparator comparator;
        DomainReceiverHeap heaps[3];
    public:
        FreelyRoutableDomainGreedyRouter(RoutingGraphBuilder& builder, SharedPtr<platform::InterruptDomain>& domain, DomainReceiverLoadMap& loads);
        Optional<RoutingGraphBuilder::EdgeHandle> route(size_t receiverIndex);
        bool routeAll();
    };

    class InterruptRoutingPolicy{
    public:
        virtual ~InterruptRoutingPolicy() = default;
        virtual RoutingGraph buildRoutingGraph(RoutingGraphBuilder& builder) = 0;
    };

    class GreedyRoutingPolicy : public InterruptRoutingPolicy {
    public:
        RoutingGraph buildRoutingGraph(RoutingGraphBuilder& builder) override;
        ~GreedyRoutingPolicy() override = default;
    };
}

#endif //CROCOS_INTERRUPTROUTINGPOLICY_H