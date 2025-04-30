//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
namespace kernel::hal::interrupts{
    namespace topology{
        void doNothing(InterruptSource){
            //Could do a log here or something idk
        }
    }
}

using namespace kernel::hal::interrupts;
Core::PrintStream& operator<<(Core::PrintStream& ps, NontargetedAffinityTypes& affinityType){
    switch (affinityType) {
        case NontargetedAffinityTypes::Global: ps << "Global"; break;
        case NontargetedAffinityTypes::RoundRobin: ps << "Round Robin"; break;
        case NontargetedAffinityTypes::LocalProcessor: ps << "Local Processor"; break;
    }
    return ps;
}