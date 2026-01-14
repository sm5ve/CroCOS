//
// Created by Spencer Martin on 1/13/26.
//

#include <init.h>
#include <kernel.h>
#include <arch.h>

namespace kernel::init {
    void resetComponentStates() {
        size_t i = 0;
        for (const InitComponent* component = &init_components[0]; *component != END_SENTINEL; component++) {
            complete_components[i] = false;
            i++;
        }
    }

    bool shouldPrintComponent(const InitComponent& component, LoggingImportance importance) {
        if (importance == LoggingImportance::DEBUG) {
            return true;
        }
        if (component.logging_importance == LoggingImportance::DEBUG) {
            return false;
        }
        return component.logging_importance >= importance;
    }

    bool shouldPrintError(const InitComponent& component, LoggingImportance importance) {
        if (importance == LoggingImportance::DEBUG) {
            return true;
        }
        return component.logging_importance != LoggingImportance::DEBUG;
    }

    void printAPBadge(bool phaseMarker, bool hasPID) {
        klog << "[AP ";
        if (hasPID) {
            klog << arch::getCurrentProcessorID();
        }
        else {
            klog << "?";
        }
        if (phaseMarker) {
            klog << " Phase";
        }
        klog << "] ";
    }

    void kinit(bool bootstrap, LoggingImportance minimalComponentImportance, bool logBootstrapProcessorOnly) {
        if (bootstrap) {
            resetComponentStates();
        }
        for (size_t i = 0;; i++) {
            const InitComponent& component = init_components[i];
            if (component == END_SENTINEL) break;
            if ((component.flags) & CF_PHASE_MARKER) {
                if (shouldPrintComponent(component, minimalComponentImportance)) {
                    if (bootstrap) {
                        klog << "[BSP Phase] " << component.name << "\n";
                    }
                    if (!logBootstrapProcessorOnly && !bootstrap) {
                        printAPBadge(true, component.flags & CF_AP_ID_AVAILABLE);
                        klog << component.name << "\n";
                    }
                }
                continue;
            }
            if ((component.flags) & CF_PER_CPU) {
                if (bootstrap) {
                    if (shouldPrintComponent(component, minimalComponentImportance)) {
                        klog << "[BSP] " << component.name << "\n";
                    }
                    bool succeeded = component.bootstrap_initializer();
                    if (!succeeded && shouldPrintError(component, minimalComponentImportance)) {
                        klog << "Failed to initialize " << component.name << "\n";
                        if (component.flags & CF_REQUIRED) {
                            assertNotReached("Failed to initialize required component");
                        }
                    }
                }
                else {
                    if (shouldPrintComponent(component, minimalComponentImportance)) {
                        printAPBadge(false, component.flags & CF_AP_ID_AVAILABLE);
                        klog << component.name << "\n";
                    }
                    bool succeeded = component.ap_initializer();
                    if (!succeeded && shouldPrintError(component, minimalComponentImportance)) {
                        klog << "Failed to initialize " << component.name << "\n";
                        if (component.flags & CF_REQUIRED) {
                            assertNotReached("Failed to initialize required component");
                        }
                    }
                }
            }
            else {
                if (bootstrap) {
                    if (shouldPrintComponent(component, minimalComponentImportance)) {
                        klog << "[BSP] " << component.name << "\n";
                    }
                    bool succeeded = component.bootstrap_initializer();
                    if (!succeeded && shouldPrintError(component, minimalComponentImportance)) {
                        klog << "Failed to initialize " << component.name << "\n";
                        if (component.flags & CF_REQUIRED) {
                            assertNotReached("Failed to initialize required component");
                        }
                    }
                    complete_components[i] = true;
                }
                else {
                    while (!complete_components[i]) {
                        tight_spin();
                    }
                }
            }
        }
    }
}
