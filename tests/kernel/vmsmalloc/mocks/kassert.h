//
// vmsmalloc Phase 8 — mock <kassert.h> for the userspace harness.
//
// vmsmalloc.cpp includes <kassert.h> directly; the real one maps assert to
// PANIC (which, on the ARM host, is effectively a no-op after the x86-only
// quit sequence is skipped). The harness instead wants asserts to throw
// CroCOSTest::AssertionFailure — so unexpected assert violations surface as
// clean test failures and the (deferred) negative tests can catch them.
//
// The macro bodies are intentionally byte-identical to Core's testing assert
// (libraries/Core/include/assert.h, CORE_LIBRARY_TESTING branch): vmsmalloc.cpp
// transitively includes both this header and Core's <assert.h>, and identical
// macro redefinition is well-formed. Uses the real CROCOS_ASSERT_H guard so it
// fully shadows the kernel header.
//

#ifndef CROCOS_ASSERT_H
#define CROCOS_ASSERT_H

#include <panic.h>            // PANIC -> kernel::panic (stubbed; never hit on valid paths)
#include <assert_support.h>  // CroCOSTest::AssertionFailure, formatAssertMessage

#define assert(condition, ...) \
    do { \
        if (!(condition)) { \
            throw CroCOSTest::AssertionFailure(CroCOSTest::formatAssertMessage("Assert failed: ", __VA_ARGS__)); \
        } \
    } while(0)

#define assertNotReached(...) \
    do { \
        throw CroCOSTest::AssertionFailure(CroCOSTest::formatAssertMessage("Assert not reached: ", __VA_ARGS__)); \
    } while(0)

#define assertUnimplemented(...) \
    do { \
        throw CroCOSTest::AssertionFailure(CroCOSTest::formatAssertMessage("Assert unimplemented: ", __VA_ARGS__)); \
    } while(0)

// No-op in the harness (the real macro is a compile-time hack-expiry guard).
#define temporaryHack(d, m, y, message)

#endif // CROCOS_ASSERT_H
