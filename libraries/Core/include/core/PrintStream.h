//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_PRINTSTREAM_H
#define CROCOS_PRINTSTREAM_H

#include <stdint.h>
#include "utility.h"

namespace Core{
    class PrintStream{
    protected:
        virtual void putString(const char*) = 0;  // Make it pure virtual if meant to be overridden

    public:
        PrintStream& operator<<(const char);
        PrintStream& operator<<(const char*);
        PrintStream& operator<<(const void*);
        PrintStream& operator<<(const uint8_t);
        PrintStream& operator<<(const uint16_t);
        PrintStream& operator<<(const uint32_t);
        PrintStream& operator<<(const uint64_t);
        PrintStream& operator<<(const int16_t);
        PrintStream& operator<<(const int32_t);
        PrintStream& operator<<(const int64_t);
        PrintStream& operator<<(const bool);
    };

    template<typename T>
    concept Printable = requires(T t, PrintStream& ps)
    {
        ps << t;
    };

#ifdef CROCOS_TESTING
    PrintStream& cout();
#endif
}

#endif //CROCOS_PRINTSTREAM_H
