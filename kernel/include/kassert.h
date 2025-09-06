//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_ASSERT_H
#define CROCOS_ASSERT_H

#include <panic.h>

// Format of __DATE__: "Apr 12 2025"
constexpr int parseMonth(const char* date) {
    // Very crude, just enough for quick dirty checks
    if (date[0] == 'A' && date[1] == 'p') return 4;
    if (date[0] == 'M' && date[1] == 'a' && date[2] == 'r') return 3;
    if (date[0] == 'M' && date[1] == 'a' && date[2] == 'y') return 5;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'n') return 6;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'l') return 7;
    if (date[0] == 'A' && date[1] == 'u') return 8;
    if (date[0] == 'S') return 9;
    if (date[0] == 'O') return 10;
    if (date[0] == 'N') return 11;
    if (date[0] == 'D') return 12;
    if (date[0] == 'F') return 2;
    if (date[0] == 'J') return 1;
    return 0;
}

constexpr int parseDay(const char* date) {
    return (date[4] == ' ' ? date[5] - '0' : (date[4] - '0') * 10 + (date[5] - '0'));
}

constexpr int parseYear(const char* date) {
    return (date[7] - '0') * 1000 + (date[8] - '0') * 100 +
           (date[9] - '0') * 10 + (date[10] - '0');
}

constexpr bool before(int y, int m, int d) {
    constexpr const char* date = __DATE__;
    int curY = parseYear(date);
    int curM = parseMonth(date);
    int curD = parseDay(date);
    return (curY < y) || (curY == y && curM < m) || (curY == y && curM == m && curD < d);
}

#ifdef DEBUG_BUILD
#define assert_base(condition, ...) if(!(condition)) PANIC(__VA_ARGS__)
#define assert(condition, ...) assert_base((condition), "Assert failed: ", __VA_ARGS__)
#define assertNotReached(...) assert_base(false, "Assert not reached ", __VA_ARGS__)
#define assertUnimplemented(...) assert_base(false, "Assert unimplemented: ", __VA_ARGS__)
#else
#define assert(condition, message, ...) (void)(condition)
#define assertNotReached(message)
#define assertUnimplemented(message)
#endif
#define temporaryHack(d, m, y, message) static_assert(before(y, m, d), "Hack expired: " message)

#endif //CROCOS_ASSERT_H
