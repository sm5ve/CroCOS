//
// Created by Spencer Martin on 6/30/25.
//

#include <core/Object.h>

extern "C" void (*__crocos_presort_array_start[])(void) __attribute__((weak));
extern "C" void (*__crocos_presort_array_end[])(void) __attribute__((weak));

void presort_object_parent_lists(){
    static bool initialized = false;
    if (initialized) return;
    for (void (**presort)() = __crocos_presort_array_start; presort != __crocos_presort_array_end; presort++) {
        (*presort)();
    }
}