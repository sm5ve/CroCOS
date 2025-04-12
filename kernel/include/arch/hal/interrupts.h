//
// Created by Spencer Martin on 4/10/25.
//

#ifndef CROCOS_INTERRUPTS_H
#define CROCOS_INTERRUPTS_H

//#include <lib/ds/Vector.h>
#include "arch/hal/hal.h"

namespace kernel::hal::interrupts{
    namespace topology{
        enum TriggerType{
            LEVEL_HIGH,
            LEVEL_LOW,
            EDGE_RISING,
            EDGE_FALLING
        };

        inline bool isLevelTriggered(TriggerType t){
            return (t == LEVEL_HIGH) || (t == LEVEL_LOW);
        }

        inline bool isEdgeTriggered(TriggerType t){
            return !isLevelTriggered(t);
        }

        class IInterruptSource;

        class IInterruptReceiver{
        public:
            virtual bool isTerminal() = 0;
            virtual TriggerType getTriggerType() = 0;
            virtual IInterruptSource* getOutput() = 0;
        };

        class IInterruptSource{
        private:
            IInterruptReceiver* receiver = nullptr;
        public:
            virtual bool interruptRequested() = 0;
            virtual void acknowledgeInterrupt() = 0;
            virtual uint32_t getLineNumber() = 0;
            void setReceiver(IInterruptReceiver* r){receiver = r;};
            IInterruptReceiver* getReceiver(){return receiver;};
        };

        class IInterruptReceiverBus{
        public:
            virtual uint32_t inputLineCount() = 0;
            virtual IInterruptReceiver* getInputForLine(uint32_t) = 0;
        };

        class InterruptSourceBus{
        private:
            //In the future we may want the ability to connect multiple receiver buses to a single source bus
            //For the time being, though, we only support connecting a single bus for the sake of simplicity
            //The upshot is that most changes needed to support multiple buses should be localized to the
            //implementation of this one class
            IInterruptReceiverBus* connectedBus;
        public:
            InterruptSourceBus(uint32_t maxWidth);
            void connectBus(IInterruptReceiverBus& bus, uint32_t offsetIntoBus, uint32_t totalLinesExposed);
            uint32_t getOutputLineCount();
            IInterruptReceiver* getLine(uint32_t index);
        };

        class IInterruptController : public IInterruptReceiverBus{
        public:
            enum Capabilities{
                DEFAULT,
                FIXED_CPU_AFFINITY,
                GLOBAL_CONFIGURABLE_CPU_AFFINITY,
                PER_LINE_CONFIGURABLE_CPU_AFFINITY
            };
            virtual InterruptSourceBus& getOutputBus() = 0;
            //Returns true on success, false on failure
            virtual bool setMapping(IInterruptReceiver&, IInterruptSource&) = 0;
            //Returns the prior state of the interrupt mask
            virtual bool enableInterrupt(IInterruptReceiver& line) = 0;
            virtual bool disableInterrupt(IInterruptReceiver& line) = 0;
            //Returns false if the trigger type is unsupported or otherwise failed to set
            virtual bool setTriggerType(IInterruptReceiver& line, TriggerType type) = 0;
            virtual Capabilities getControllerCapabilities() = 0;
        };

        class IFixedCPUAffinityInterruptController : public IInterruptController{
        public:
            virtual kernel::hal::ProcessorID getAffinity() = 0;
        };

        class IConfigurableGlobalCPUAffinityInterruptController : public IInterruptController{
        public:
            virtual bool hasAssignedAffinity() = 0;
            virtual kernel::hal::ProcessorID getAffinity() = 0;
            virtual void setAffinity(kernel::hal::ProcessorID pid) = 0;
        };

        class IConfigurableLineCPUAffinityInterruptController : public IInterruptController{
        public:
            virtual bool lineHasAssignedAffinity(uint32_t lineNumber) = 0;
            virtual kernel::hal::ProcessorID getAffinity(uint32_t lineNumber) = 0;
            virtual void setAffinity(uint32_t lineNumber, kernel::hal::ProcessorID pid) = 0;
        };
    }
}

#endif //CROCOS_INTERRUPTS_H
