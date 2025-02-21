//
// Created by Spencer Martin on 2/15/25.
//

#include <lib/str.h>

const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";

bool startsWith(const char* str, const char* prefix){
    while(*prefix){
        if(*str != *prefix){
            return false;
        }
        str++;
        prefix++;
    }
    return true;
}