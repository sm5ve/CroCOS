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

// WITH_GLOBAL_CONSTRUCTOR / ARRAY_WITH_GLOBAL_CONSTRUCTOR used to live here.
// They hand-rolled a function pointer into .init_array and placement-new'd the
// object, because a stock x86_64-elf cross compiler emits global constructors
// into legacy .ctors sections that the kernel's linker script never collected.
//
// They are gone because the toolchain now does this correctly -- and because
// they became actively harmful once it did. The macro declared `static Type
// name;` and then constructed over it, so for any Type with a non-trivial
// default constructor GCC emitted its own initializer as well. That initializer
// used to be silently discarded along with the rest of .ctors; with a working
// .init_array it runs too, constructing every such object twice.
//
// Write plain globals. See tools/toolchain/README.md.

#endif //CROCOS_KERNEL_H