//
// Created by Spencer Martin on 1/8/26.
//

#include <timing.h>

namespace kernel::timing {
    void blockingSleep(uint64_t ms) {
        volatile bool sleeping = true;
        enqueueEvent([&]{sleeping = false;}, ms);
        while (sleeping) {
            asm volatile("hlt");
        }
    }

    void sleepns(uint64_t ns) {
        uint64_t expectedTime = monoTimens() + ns;
        while (monoTimens() < expectedTime) {
            asm volatile("pause");
        }
    }
}