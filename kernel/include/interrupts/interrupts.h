//
// Created by Spencer Martin on 7/27/25.
//

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

#include <interrupts/InterruptGraphs.h>
#include <interrupts/InterruptRoutingPolicy.h>

#include <arch.h>

namespace kernel::interrupts {
    namespace managed {
        using InterruptHandler = Function<void(arch::InterruptFrame&)>;
        using InterruptSourceHandle = RoutingNodeLabel;

        bool updateRouting();
        void dispatchInterrupt(arch::InterruptFrame& frame);
        void registerHandler(const InterruptSourceHandle& interruptSource, InterruptHandler&& handler);
    }
}

#include <interrupts/_InterruptsExplicitTypes.h>

#endif //INTERRUPTS_H
