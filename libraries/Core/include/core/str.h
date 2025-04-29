//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_STR_H
#define CROCOS_STR_H

extern const char* digits;// = "0123456789abcdefghijklmnopqrstuvwxyz";

template <class T>
int itoa(T value, char* str, int base){
    if(value == 0){
        str[0] = '0';
        str[1] = 0;
        return 1;
    }
    if(value < 0){
        str[0] = '-';
        str++;
        value *= -1;
    }
    int len = 0;
    while(value != 0){
        str[len] = digits[value % base];
        value /= (T)base;
        len++;
    }
    for(int i = 0; i < len/2; i++){
        char a = str[i];
        char b = str[len - i - 1];
        str[len - i - 1] = a;
        str[i] = b;
    }
    str[len] = 0;
    return len;
}

template <class T>
void paddedItoa(T value, char* str, int base, int length){
    for(int i = 0; i < length; i++){
        str[i] = '0';
    }
    str[length] = 0;
    if(value == 0){
        return;
    }
    if(value < 0){
        str[0] = '-';
        str++;
        length--;
        value *= -1;
    }
    int n = 0;
    while(value != 0){
        str[n++] = digits[value % base];
        value /= base;
    }
    for(int i = 0; i < length/2; i++){
        char a = str[i];
        char b = str[length - i - 1];
        str[length - i - 1] = a;
        str[i] = b;
    }
}

bool startsWith(const char* str, const char* sub);

#endif //CROCOS_STR_H
