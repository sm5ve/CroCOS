//
// Created by Spencer Martin on 8/29/25.
//

#ifndef CROCOS_INTERRUPTROUTINGPOLICY_H
#define CROCOS_INTERRUPTROUTINGPOLICY_H

#include <arch/hal/InterruptGraphs.h>

namespace kernel::hal::interrupts::managed{
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