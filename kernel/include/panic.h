//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_PANIC_H
#define CROCOS_PANIC_H

#define PANIC(x) kernel::panic(x, __FILE__, __LINE__)

namespace kernel{
    [[noreturn]]
    void panic(const char* message, const char* filename, const uint32_t line);
    void print_stacktrace();
}

#endif //CROCOS_PANIC_H