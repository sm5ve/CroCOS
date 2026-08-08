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
    // ─── klog is NOT safe from interrupt context ───────────────────────────
    //
    // `klog()` returns a temporary that holds a **global spinlock for its whole
    // lifetime**, so a `klog() << a << b << c;` statement is one critical
    // section spanning every `<<`. That is what makes a log line atomic against
    // other CPUs, and it is also why the call is not reentrant: an interrupt
    // taken on a CPU that is mid-statement, whose handler logs, re-acquires a
    // lock that CPU already holds.
    //
    // The failure is worse than it looks. In a debug build the spinlock's
    // deadlock detector turns it into a PANIC, which is at least loud. In
    // release the detector is compiled out and the same interleaving is a
    // **silent hard hang with the log lock held** — and a hang watchdog cannot
    // report it, because the watchdog is itself a timer event on the CPU that is
    // now spinning inside an interrupt handler.
    //
    // So: **timer callbacks, interrupt handlers and panic paths use
    // `kernel::emergencyLog()`** (declared in `panic.h`), never this. Found the hard way — see
    // `docs/radix-tree-implementation-deviations.md` D-043, where the shutdown
    // timer's "Goodbye :)" landed inside an `rcu: domain ready` line.
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