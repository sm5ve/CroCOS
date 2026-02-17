//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_KERNEL_H
#define CROCOS_KERNEL_H

#include <core/PrintStream.h>
#include "stddef.h"
#include <core/mem.h>
#include <core/utility.h>

#ifdef HOSTED
#include <new>
#endif

namespace kernel{
    Core::AtomicPrintStream klog();
    bool heapEarlyInit();
    void* kmalloc(size_t size, std::align_val_t = std::align_val_t{1});
    void kfree(void* ptr);
}

#include <assert.h>

//Kinda hacky - what appears to be a call to the constructor name(__VA_ARGS__) does nothing
//but it tricks the compiler into going forward with compilation. The name##_init() is what actually
//calls the constructor.
//This is a seemingly necessary hack to prevent having to recompile crtstart/end with the kernel memory model
//and linking it in with the kernel as suggested by https://forum.osdev.org/viewtopic.php?t=28066
#ifndef CROCOS_TESTING
#define WITH_GLOBAL_CONSTRUCTOR(Type, name, ...)                                  \
    __attribute__((used)) static Type name __VA_ARGS__;                           \
    static void name##_init() {                                                   \
        static bool initialized = false;                                          \
        assert(!initialized, "Double-initialized ", #name);                       \
        initialized = true;                                                       \
        new (& name) Type(__VA_ARGS__);                                           \
    }                                                                             \
    static void (*name##_ctor)(void) __attribute__((used, section(".init_array"))) = name##_init;

#define ARRAY_WITH_GLOBAL_CONSTRUCTOR(Type, size, name)                           \
    __attribute__((used)) static Type name[size];                                 \
    static void name##_init() {                                                   \
        static bool initialized = false;                                          \
        assert(!initialized, "Double-initialized ", #name);                       \
        initialized = true;                                                       \
        for (auto& x : (name)) new (& x) Type();                                  \
    }                                                                             \
    static void (*name##_ctor)(void) __attribute__((used, section(".init_array"))) = name##_init;
#endif
#endif //CROCOS_KERNEL_H