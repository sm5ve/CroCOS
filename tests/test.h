//
// Created by Spencer Martin on 7/24/25.
//

#ifndef CROCOS_TEST_H
#define CROCOS_TEST_H

#include <stddef.h>  // For size_t
#include "assert_support.h"
#include "harness/MemoryTrackingGuard.h"

#define WITH_GLOBAL_CONSTRUCTOR(a, b) a b

// Only define dummy allocators when instrumenting library code for objcopy
// Test files should use real allocators from the standard library
#ifdef CROCOS_TEST_INSTRUMENT_ALLOCATORS

extern volatile void* __garbage;

extern "C"{
    void* malloc(size_t size){ (void)size; return const_cast<void*>(__garbage); }
    void* calloc(size_t count, size_t size){ (void)count; (void)size; return const_cast<void*>(__garbage); }
    void* realloc(void* old_ptr, size_t size){ (void)old_ptr; (void)size; return const_cast<void*>(__garbage); }
    void free(void* ptr) { (void)ptr; }
}

void* operator new(size_t size){ (void)size; return const_cast<void*>(__garbage); }
void* operator new[](size_t size){ (void)size; return const_cast<void*>(__garbage); }
void operator delete(void* ptr) noexcept { (void)ptr; }
void operator delete[](void* ptr) noexcept { (void)ptr; }
void operator delete(void* ptr, size_t size) noexcept { (void)ptr; (void)size; }
void operator delete[](void* ptr, size_t size) noexcept { (void)ptr; (void)size; }

#endif // CROCOS_TEST_INSTRUMENT_ALLOCATORS

#endif // CROCOS_TEST_H