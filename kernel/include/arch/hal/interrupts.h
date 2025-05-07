//
// Created by Spencer Martin on 4/10/25.
//

#ifndef CROCOS_INTERRUPTS_H
#define CROCOS_INTERRUPTS_H

#include <core/ds/Vector.h>
#include <core/ds/Tuple.h>
#include <arch/hal/hal.h>
#include <core/ds/Variant.h>
#include <core/ds/Optional.h>
#include <core/ds/HashMap.h>
#include "core/ds/Handle.h"
#include <core/utility.h>

namespace kernel::hal::interrupts{
    enum NontargetedAffinityTypes{
        Global,
        RoundRobin,
        LocalProcessor //For things like the APIC timer/LAPIC lines
    };

    using InterruptCPUAffinity = Variant<hal::ProcessorID, NontargetedAffinityTypes>;

    enum class ActivationType : uint8_t{
        LevelHigh,
        LevelLow,
        EdgeHigh,
        EdgeLow
    };

    enum class InterruptMaskState : uint8_t{
        Unmasked, //Interrupts will go through and call the associated handler
        Ignored, //The line for the interrupt controller is shared, and the other interrupt sources are unmasked
        Masked //Interrupts are masked in the controller
    };

    namespace hardware{
        class IInterruptController;
    };

    struct InterruptReceiver{
        hardware::IInterruptController* owner;
        uint64_t metadata;

        bool operator==(const InterruptReceiver& other) const {return owner == other.owner && metadata == other.metadata;}
    };

    struct InterruptSource{
        hardware::IInterruptController* owner;
        uint64_t metadata;

        bool operator==(const InterruptSource& other) const {return owner == other.owner && metadata == other.metadata;}
    };

    bool doNothing(InterruptReceiver);

    namespace hardware{
        using VectorIndex = uint32_t;

        enum class InterruptControllerFeature : uint32_t {
            FixedVectorRouting          = 1 << 0, //Routes directly to interrupt vectors
            AffinityLocallyConfigurable = 1 << 1, //Can route to specific CPU cores or groups
            AffinityFixed               = 1 << 2, //Has a fixed CPU affinity, as would be the case for a LAPIC
            SupportsLevelTrigger        = 1 << 3, //Edge vs level-triggering support
            CascadedController          = 1 << 4 //Is a child controller
        };

        inline InterruptControllerFeature operator|(InterruptControllerFeature lhs, InterruptControllerFeature rhs) {
            return static_cast<InterruptControllerFeature>(
                    static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
        }

        inline InterruptControllerFeature operator&(InterruptControllerFeature lhs, InterruptControllerFeature rhs) {
            return static_cast<InterruptControllerFeature>(
                    static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
        }

        inline InterruptControllerFeature& operator|=(InterruptControllerFeature& lhs, InterruptControllerFeature rhs) {
            lhs = lhs | rhs;
            return lhs;
        }

        inline bool hasFeature(InterruptControllerFeature set, InterruptControllerFeature feature) {
            return static_cast<uint32_t>(set & feature) != 0;
        }

        const InterruptControllerFeature terminalController = InterruptControllerFeature::FixedVectorRouting | InterruptControllerFeature::AffinityLocallyConfigurable | InterruptControllerFeature::AffinityFixed;

        class IInterruptController{
        public:
            virtual ~IInterruptController() = default;
            virtual InterruptControllerFeature getFeatureSet() = 0;
            virtual bool setInterruptMaskState(InterruptReceiver receiver, bool) = 0;
            virtual bool getInterruptMaskState(InterruptReceiver receiver) = 0;
        };

        class IConfigurableTriggerController{
        public:
            virtual ~IConfigurableTriggerController() = default;
            virtual ActivationType getTriggerMode(InterruptReceiver receiver) = 0;
        };

        class ISubordinateController{
        public:
            virtual ~ISubordinateController() = default;
            virtual Optional<InterruptSource> getMapping(InterruptReceiver r) = 0;
            virtual bool setMapping(InterruptReceiver r, Optional<InterruptSource>) = 0;
        };

        class ITerminalController{
        public:
            virtual ~ITerminalController() = default;
            virtual Optional<Tuple<VectorIndex, Optional<InterruptCPUAffinity>>> getMapping(InterruptReceiver r) = 0;
            virtual bool setMapping(InterruptReceiver r, VectorIndex, Optional<InterruptCPUAffinity>) = 0;
        };
    }

    namespace managed{
        enum InterruptHandlerResult{
            Serviced,
            Deferred,
            Unmatched
        };

        class IInterruptHandler {
        public:
            virtual InterruptHandlerResult operator()(hal::InterruptFrame&) = 0;
            virtual bool operator==(IInterruptHandler& h) const = 0;
            virtual uint64_t getTypeID() const = 0;
        };

        class InterruptHandler : public IInterruptHandler{
        private:
            using InterruptHandlerLambda = FunctionRef<InterruptHandlerResult(hal::InterruptFrame&)>;
            InterruptHandlerLambda handler;
        public:
            InterruptHandler(InterruptHandlerLambda& h) : handler(h){}
            virtual InterruptHandlerResult operator()(hal::InterruptFrame& frame) override{
                return handler(frame);
            }

            virtual uint64_t getTypeID() const override{
                return TypeID_v<InterruptHandler>;
            }

            virtual bool operator==(IInterruptHandler& h) const override {
                if(h.getTypeID() != this->getTypeID()) return false;
                auto arg = reinterpret_cast<InterruptHandler &>(h);
                return arg.handler == handler;
            }
        };

        bool registerHandler(InterruptSource, IInterruptHandler&);
        bool unregisterHandler(InterruptSource, IInterruptHandler&);
        bool setCPUAffinity(InterruptSource, InterruptCPUAffinity);
        InterruptCPUAffinity getAffinity(InterruptSource);
        InterruptMaskState getInterruptMaskState(InterruptSource);
        bool isInterruptRaised(InterruptSource);
        bool acknowledge(InterruptSource);

        void recomputeVectorAssignments();
        //Used on x86 for reserving vectors 0-31 for exceptions, as well as a vector for syscalls
        Vector<Tuple<InterruptSource, hardware::VectorIndex>> reserveVectorRange(hardware::VectorIndex start, hardware::VectorIndex end);
        void handleInterrupt(hal::InterruptFrame& frame);
    }

    namespace topology{
        struct InterruptGroupHandleTag{};
        using InterruptGroupHandle = Handle<InterruptGroupHandleTag>;

        void registerController(hardware::IInterruptController&);
        void registerSource(InterruptSource, Optional<InterruptGroupHandle> = {});
        void bindSourceToLine(InterruptSource, InterruptReceiver);
    }
}

template<>
struct DefaultHasher<kernel::hal::interrupts::InterruptSource>{
    size_t operator()(const kernel::hal::interrupts::InterruptSource& key) const {
        return static_cast<size_t>(key.metadata) + static_cast<size_t>((uint64_t)key.owner);
    }
};

template<>
struct DefaultHasher<kernel::hal::interrupts::InterruptReceiver>{
    size_t operator()(const kernel::hal::interrupts::InterruptReceiver& key) const {
        return static_cast<size_t>(key.metadata) ^ static_cast<size_t>((uint64_t)key.owner << 1);
    }
};

Core::PrintStream& operator<<(Core::PrintStream& ps, kernel::hal::interrupts::NontargetedAffinityTypes& affinityType);


#endif //CROCOS_INTERRUPTS_H
