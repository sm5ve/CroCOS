//
// Created by Spencer Martin on 6/30/25.
//

#include <core/Object.h>

#ifdef __APPLE__
// On macOS, we need to use getsectiondata to access the custom section
    #include <mach-o/getsect.h>
    #include <mach-o/dyld.h>
    #include <iostream>

void presort_object_parent_lists() {
    // Get the main executable's mach header (index 0 is always the main executable)
    const struct mach_header_64* header = (const struct mach_header_64*)_dyld_get_image_header(0);

    if (!header) {
        return;
    }

    unsigned long size;

    void (**__crocos_presort_array_start)(void) =
        (void (**)(void))getsectiondata(header, "__DATA", "crocos_presort", &size);

    if (__crocos_presort_array_start) {
        unsigned long count = size / sizeof(void (*)(void));
        for (unsigned long i = 0; i < count; i++) {
            (__crocos_presort_array_start[i])();
        }
    }
}
#else
extern "C" void (*__crocos_presort_array_start[])(void) __attribute__((weak));
extern "C" void (*__crocos_presort_array_end[])(void) __attribute__((weak));

void presort_object_parent_lists(){
    static bool initialized = false;
    if (initialized) return;
    for (void (**presort)() = __crocos_presort_array_start; presort != __crocos_presort_array_end; presort++) {
        (*presort)();
    }
}
#endif