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
    enum InterruptLineActivationType : uint8_t{
        LEVEL_LOW = 0b00,
        LEVEL_HIGH = 0b01,
        EDGE_LOW = 0b10,
        EDGE_HIGH = 0b11
    };

    constexpr InterruptLineActivationType activationTypeForLevelAndTriggerMode(const bool activeHigh, const bool edgeTriggered) {
        uint8_t bits = 0;
        if (activeHigh) bits |= 1;
        if (edgeTriggered) bits |= 2;
        return static_cast<InterruptLineActivationType>(bits);
    }

    constexpr bool isLevelTriggered(const InterruptLineActivationType type){
        return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::LEVEL_HIGH;
    }

    constexpr bool isEdgeTriggered(const InterruptLineActivationType type){
        return !isLevelTriggered(type);
    }

    constexpr bool isLowTriggered(const InterruptLineActivationType type){
        return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::EDGE_LOW;
    }

    constexpr bool isHighTriggered(const InterruptLineActivationType type){
        return !isLowTriggered(type);
    }

    namespace managed {
        using InterruptHandler = FunctionRef<void(hal::InterruptFrame&)>;
        using InterruptSourceHandle = RoutingNodeLabel;

        void updateRouting();
        void dispatchInterrupt(InterruptFrame& frame);
        void registerHandler(const InterruptSourceHandle& interruptSource, UniquePtr<InterruptHandler>&& handler);
    }
}

#endif //INTERRUPTS_H
