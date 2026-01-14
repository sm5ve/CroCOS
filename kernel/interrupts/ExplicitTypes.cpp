//
// Created by Spencer Martin on 9/5/25.
//

#include <interrupts/interrupts.h>
#include <core/ds/SmartPointer.h>

template class SharedPtr<kernel::interrupts::platform::InterruptDomain>;
template class SharedPtr<kernel::interrupts::platform::RoutableDomain>;
template class SharedPtr<kernel::interrupts::platform::InterruptReceiver>;
template class SharedPtr<kernel::interrupts::platform::InterruptEmitter>;
template class SharedPtr<kernel::interrupts::platform::CPUInterruptVectorFile>;
template class SharedPtr<kernel::interrupts::platform::FreeRoutableDomain>;
template class SharedPtr<kernel::interrupts::platform::FixedRoutingDomain>;
template class SharedPtr<kernel::interrupts::platform::ContextIndependentRoutableDomain>;
template class SharedPtr<kernel::interrupts::platform::ConfigurableActivationTypeDomain>;
template class SharedPtr<kernel::interrupts::platform::EOIDomain>;
template class Vector<SharedPtr<kernel::interrupts::platform::EOIDomain>>;
