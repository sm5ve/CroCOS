//
// Created by Spencer Martin on 7/27/25.
//

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

#include <arch/hal/InterruptGraphs.h>
#include <arch/hal/InterruptRoutingPolicy.h>

#include <arch/hal/hal.h>

namespace kernel::hal::interrupts {
    namespace managed {
        using InterruptHandler = Function<void(hal::InterruptFrame&)>;
        using InterruptSourceHandle = RoutingNodeLabel;

        void updateRouting();
        void dispatchInterrupt(InterruptFrame& frame);
        void registerHandler(const InterruptSourceHandle& interruptSource, InterruptHandler&& handler);
    }
}

#include <arch/hal/internal/_InterruptsExplicitTypes.h>

#endif //INTERRUPTS_H
