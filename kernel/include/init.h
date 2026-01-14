//
// Created by Spencer Martin on 1/13/26.
//

#ifndef CROCOS_INIT_H
#define CROCOS_INIT_H

#include <stdint.h>
#include <core/atomic.h>

namespace kernel::init{

    using ComponentFlag = uint8_t;
    using Initializer = bool (*)();

    constexpr ComponentFlag CF_NONE = 0;
    constexpr ComponentFlag CF_REQUIRED = 1;
    constexpr ComponentFlag CF_PER_CPU = 2;
    constexpr ComponentFlag CF_PHASE_MARKER = 4;
    constexpr ComponentFlag CF_AP_ID_AVAILABLE = 8;

    enum class LoggingImportance : uint8_t{
        DEBUG = 0,
        IMPORTANT = 1,
        CRITICAL = 2,
        ERROR = 3
    };

    struct InitComponent{
        const char* name;
        Initializer bootstrap_initializer;
        Initializer ap_initializer;
        ComponentFlag flags;
        LoggingImportance logging_importance;

        bool operator==(const InitComponent& other) const {
            return (name == other.name) && (bootstrap_initializer == other.bootstrap_initializer) &&
                (ap_initializer == other.ap_initializer) && (flags == other.flags) &&
                    (logging_importance == other.logging_importance);
        }
    };

    const InitComponent END_SENTINEL = {nullptr, nullptr, nullptr, CF_NONE, LoggingImportance::DEBUG};
}

namespace kernel::init {
    extern const InitComponent init_components[];
    extern Atomic<bool> complete_components[];
    void kinit(bool bootstrap, LoggingImportance minimalComponentImportance, bool logBootstrapProcessorOnly);
}

#endif //CROCOS_INIT_H