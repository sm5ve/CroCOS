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
#include <core/ds/Handle.h>
#include <core/utility.h>
#include <core/Object.h>
#include <core/ds/SmartPointer.h>

namespace kernel::hal::interrupts {
    namespace backend {
        namespace platform {
            class IInterruptDomain;

            struct DomainInput {
                const uint64_t index;
                const IInterruptDomain* domain;

                bool operator==(const DomainInput& other) const;
            };

            struct DomainOutput {
                const uint64_t index;
                const IInterruptDomain* domain;

                bool operator==(const DomainOutput& other) const;
            };

            CRClass(IInterruptDomain) {

            };

            CRClass(IInterruptDomainConnector) {
                virtual ~IInterruptDomainConnector() override = default;
                [[nodiscard]]
                virtual size_t width() const = 0;
                virtual Optional<DomainOutput> getConnectedOutput(DomainInput& i) const = 0;
            };
        }
        namespace topology {
            void registerInterruptDomain(SharedPtr<platform::IInterruptDomain> domain);
            void registerDomainConnector(SharedPtr<platform::IInterruptDomainConnector> connector);
        }
    }

    class IInterruptRoutingPolicy {

    };

    namespace managed {
        enum InterruptHandlerResult{
            Serviced, //OK to issue an EOI
            Deferred, //Maybe mask the interrupt, then send EOI?
            Unmatched
        };

        CRClass(IInterruptHandler) {
        public:
            virtual ~IInterruptHandler() = default;

            virtual InterruptHandlerResult operator()(InterruptFrame&) = 0;
            virtual bool operator==(IInterruptHandler& h) const = 0;
        };

        CRClass(InterruptHandler, public IInterruptHandler){
        public:
            using InterruptHandlerLambda = FunctionRef<InterruptHandlerResult(InterruptFrame&)>;
        private:
            InterruptHandlerLambda handler;
        public:
            InterruptHandler(InterruptHandlerLambda& h) : handler(h){}

            virtual InterruptHandlerResult operator()(InterruptFrame& frame) override{
                return handler(frame);
            }

            virtual bool operator==(IInterruptHandler& h) const override {
                if(!h.instanceof(TypeID_v<InterruptHandler>)) return false;
                auto arg = reinterpret_cast<InterruptHandler &>(h);
                return arg.handler == handler;
            }
        };
    }
}

#endif //CROCOS_INTERRUPTS_H
