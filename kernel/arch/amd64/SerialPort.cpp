//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"

namespace arch::amd64{
    //Simple wrapper for the outb instruction to print out a string on the serial port
    void serialOutputString(const char* str){
        while(*str != '\0'){
            outb(0x3f8, (uint8_t)*str);
            str++;
        }
    }
}