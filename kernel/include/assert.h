//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_ASSERT_H
#define CROCOS_ASSERT_H

#include <panic.h>

#ifdef DEBUG_BUILD
#define assert(condition, message) if(!(condition)) PANIC("Assert failed: " message)
#define assertNotReached(message) assert(false, message)
#else
#define assert(condition, message)
#define assertNotReached(message)
#endif

#endif //CROCOS_ASSERT_H
