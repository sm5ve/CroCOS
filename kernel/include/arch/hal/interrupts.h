//
// Created by Spencer Martin on 7/27/25.
//

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

#include <arch/hal/InterruptGraphs.h>

enum InterruptLineActivationType : uint8_t{
    LEVEL_LOW = 0b00,
    LEVEL_HIGH = 0b01,
    EDGE_LOW = 0b10,
    EDGE_HIGH = 0b11
};

constexpr InterruptLineActivationType activationTypeForLevelAndTriggerMode(bool activeHigh, bool edgeTriggered) {
    uint8_t bits = 0;
    if (activeHigh) bits |= 1;
    if (edgeTriggered) bits |= 2;
    return static_cast<InterruptLineActivationType>(bits);
}

constexpr bool isLevelTriggered(InterruptLineActivationType type){
    return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::LEVEL_HIGH;
}

constexpr bool isEdgeTriggered(InterruptLineActivationType type){
    return !isLevelTriggered(type);
}

constexpr bool isLowTriggered(InterruptLineActivationType type){
    return type == InterruptLineActivationType::LEVEL_LOW || type == InterruptLineActivationType::EDGE_LOW;
}

constexpr bool isHighTriggered(InterruptLineActivationType type){
    return !isLowTriggered(type);
}

#endif //INTERRUPTS_H
