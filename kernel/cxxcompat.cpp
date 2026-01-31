//
// Created by Spencer Martin on 8/23/25.
//
#include "include/panic.h"

extern "C" void *__dso_handle;
void *__dso_handle = nullptr;

extern "C" int __cxa_atexit(void (*destructor) (void *), void *arg, void *___dso_handle){
    (void)destructor;
    (void)arg;
    (void)___dso_handle;
    return 0;
}

// Stack smashing protection
// The canary value contains bytes that commonly terminate strings (null, newline, 0xFF)
// to help detect string-based buffer overflows
extern "C" {
    uintptr_t __stack_chk_guard = 0x595e9fbd94fda766;

    [[noreturn]] void __stack_chk_fail(){
        PANIC_NO_STACKTRACE("Stack smashing detected!");
    }
}