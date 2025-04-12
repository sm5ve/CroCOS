//
// Created by Spencer Martin on 2/15/25.
//

#include <lib/PrintStream.h>
#include <lib/str.h>
#include "arch/hal/hal.h"

namespace kernel{

    PrintStream& PrintStream::operator<<(const char c){
        char str[2] = {c, 0};
        putString(str);
        return *this;
    }

    PrintStream& PrintStream::operator<<(const char* str){
        putString(str);
        return *this;
    }

    PrintStream& PrintStream::operator<<(const void* ptr){
        char strbuff[sizeof(uint64_t) * 2 + 1];
        paddedItoa((uint64_t)ptr, strbuff, 16, sizeof(uint64_t) * 2);
        return *this << "0x" << strbuff;
    }

    PrintStream& PrintStream::operator<<(const uint8_t x){
        char strbuff[sizeof(uint8_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const uint16_t x){
        char strbuff[sizeof(uint16_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const uint32_t x){
        char strbuff[sizeof(uint32_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const uint64_t x){
        char strbuff[sizeof(uint64_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const int16_t x){
        char strbuff[sizeof(int16_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const int32_t x){
        char strbuff[sizeof(int32_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const int64_t x){
        char strbuff[sizeof(int64_t) * 3 + 1];
        itoa(x, strbuff, 10);
        return *this << strbuff;
    }

    PrintStream& PrintStream::operator<<(const bool x){
        if(x){
            *this << "true";
        }
        else{
            *this << "false";
        }
        return *this;
    }

    void SerialPrintStream::putString(const char * str){
        kernel::hal::serialOutputString(str);
    }
}