//
// Created by Spencer Martin on 7/24/25.
//

#ifndef ASSERT_H
#define ASSERT_H

#ifdef KERNEL

#include <kassert.h>

#elif defined(CORE_LIBRARY_TESTING)

// Include necessary headers for testing
#include <string>
#include <sstream>
#include <stdexcept>

// Assert macros that integrate with test framework
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

#else

// When not in kernel or testing mode, provide empty macros
#define assert(condition, ...)
#define assertNotReached(...)
#define assertUnimplemented(...)

#endif

// temporaryHack is always available
#ifndef KERNEL
// Simplified date parsing for non-kernel builds
constexpr bool before(int, int, int) {
    return true; // Disable all temporary hacks in non-kernel builds
}
#endif

#define temporaryHack(d, m, y, message) static_assert(before(y, m, d), "Hack expired: " message)

#endif //ASSERT_H
