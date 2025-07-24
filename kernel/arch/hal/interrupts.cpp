//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
namespace kernel::hal::interrupts {
    namespace backend {
        namespace platform {
            bool DomainInput::operator==(const DomainInput &other) const {
                return index == other.index && domain == other.domain;
            }

            bool DomainOutput::operator==(const DomainOutput &other) const {
                return index == other.index && domain == other.domain;
            }
        }
    }
}