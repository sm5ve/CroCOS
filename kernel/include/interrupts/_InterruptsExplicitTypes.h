//
// Created by Spencer Martin on 9/5/25.
//

#ifndef INTERRUPTS_H
#error "Do not import this header directly"
#endif

#ifndef CROCOS_EXPLICITTYPES_H
#define CROCOS_EXPLICITTYPES_H

extern template class SharedPtr<kernel::interrupts::platform::InterruptDomain>;
extern template class SharedPtr<kernel::interrupts::platform::RoutableDomain>;
extern template class SharedPtr<kernel::interrupts::platform::InterruptReceiver>;
extern template class SharedPtr<kernel::interrupts::platform::InterruptEmitter>;
extern template class SharedPtr<kernel::interrupts::platform::CPUInterruptVectorFile>;
extern template class SharedPtr<kernel::interrupts::platform::FreeRoutableDomain>;
extern template class SharedPtr<kernel::interrupts::platform::FixedRoutingDomain>;
extern template class SharedPtr<kernel::interrupts::platform::ContextIndependentRoutableDomain>;
extern template class SharedPtr<kernel::interrupts::platform::ConfigurableActivationTypeDomain>;
extern template class SharedPtr<kernel::interrupts::platform::EOIDomain>;
extern template class Vector<SharedPtr<kernel::interrupts::platform::EOIDomain>>;

#endif //CROCOS_EXPLICITTYPES_H