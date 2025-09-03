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
        using InterruptHandler = FunctionRef<void(hal::InterruptFrame&)>;
        using InterruptSourceHandle = RoutingNodeLabel;

        void updateRouting();
        void dispatchInterrupt(InterruptFrame& frame);
        void registerHandler(const InterruptSourceHandle& interruptSource, UniquePtr<InterruptHandler>&& handler);
    }
}

#endif //INTERRUPTS_H
