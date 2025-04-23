//
// Created by Spencer Martin on 4/10/25.
//

#ifndef CROCOS_INTERRUPTS_H
#define CROCOS_INTERRUPTS_H

//#include <lib/ds/Vector.h>
#include <arch/hal/hal.h>
#include <lib/ds/Variant.h>


namespace kernel::hal::interrupts{
    namespace topology{
        class InterruptSource;
        class InterruptController;

        enum NonlocalAffinityTypes{
            Global,
            RoundRobin
        };

        using InterruptCPUAffinity = Variant<hal::ProcessorID, NonlocalAffinityTypes>;

        class InterruptReceiver{

        };

        class InterruptSource{

        };

        class InterruptController{

        };

        class ITerminalController{

        };

        class IFixedAffinityController{
        public:

        };

        class ISubordinateController{

        };
    }
}

#endif //CROCOS_INTERRUPTS_H
