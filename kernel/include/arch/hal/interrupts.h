//
// Created by Spencer Martin on 4/10/25.
//

#ifndef CROCOS_INTERRUPTS_H
#define CROCOS_INTERRUPTS_H

//#include <lib/ds/Vector.h>
#include <arch/hal/hal.h>
#include <lib/ds/Variant.h>


namespace kernel::hal::interrupts{
    enum NonlocalAffinityTypes{
        Global,
        RoundRobin
    };

    using InterruptCPUAffinity = Variant<hal::ProcessorID, NonlocalAffinityTypes>;

    enum ActivationType{
        LevelHigh,
        LevelLow,
        EdgeHigh,
        EdgeLow
    };

    enum InterruptMaskState{
        Unmasked, //Interrupts will go through and call the associated handler
        Ignored, //The line for the interrupt controller is shared, and the other interrupt sources are unmasked
        Masked //Interrupts are masked in the controller
    };

    namespace managed{
        struct InterruptSourceHandle{

        };

        enum InterruptHandlerResult{
            Serviced,
            Deferred,
            Unmatched
        };

        enum EOIPolicy{
            Automatic,
            Manual
        };

        class IInterruptHandler {
        public:
            //This will eventually take in a register file or something like that
            virtual InterruptHandlerResult operator()() = 0;
        };

        class InterruptHandler : public IInterruptHandler{
        private:
            using InterruptHandlerLambda = FunctionRef<InterruptHandlerResult()>;
            InterruptHandlerLambda handler;
        public:
            InterruptHandler(InterruptHandlerLambda& h) : handler(h){}
            virtual InterruptHandlerResult operator()() override{
                return handler();
            }
        };

        bool registerHandler(InterruptSourceHandle, IInterruptHandler);
        bool unregisterHandler(InterruptSourceHandle, IInterruptHandler);
        bool setCPUAffinity(InterruptSourceHandle, InterruptCPUAffinity);
        InterruptCPUAffinity getAffinity(InterruptSourceHandle);
        InterruptMaskState getInterruptMaskState(InterruptSourceHandle);
        bool isInterruptRaised(InterruptSourceHandle);
        bool acknowledge(InterruptSourceHandle);
        EOIPolicy setEOIPolicy(InterruptSourceHandle, EOIPolicy);
        EOIPolicy getEOIPolicy(InterruptSourceHandle);
    }

    namespace topology{
        class InterruptSource;
        class InterruptController;

        enum class InterruptControllerFeature : uint32_t {
            FixedVectorRouting          = 1 << 0, //Routes directly to interrupt vectors
            AffinityLocallyConfigurable = 1 << 1, //Can route to specific CPU cores or groups
            AffinityFixed               = 1 << 2, //Has a fixed CPU affinity, as would be the case for a LAPIC
            SupportsLevelTrigger        = 1 << 3, //Edge vs level-triggering support
            CascadedController          = 1 << 4, //Is a child controller
        };

        class IInterruptReceiver{

        };

        class IInterruptSource{
        public:
            virtual managed::InterruptSourceHandle getHandle() = 0;
        };

        class IInterruptController{
        public:
            virtual InterruptControllerFeature getFeatureSet() = 0;
        };
    }
}

#endif //CROCOS_INTERRUPTS_H
