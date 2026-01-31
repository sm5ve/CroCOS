//
// Created by Spencer Martin on 1/27/26.
//

#ifndef CROCOS_ENDIAN_H
#define CROCOS_ENDIAN_H

#ifndef __BYTE_ORDER__
#error "__BYTE_ORDER__ is not defined"
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
template <typename T>
struct LittleEndian {
    T data;

    LittleEndian(T d) : data(d) {}
    operator T() const { return data; }
    operator T&() { return data; }
    operator const T&() const { return data; }

    bool operator==(const T& rhs) const { return data == rhs; }
    bool operator!=(const T& rhs) const { return data != rhs; }
    bool operator<(const T& rhs) const { return data < rhs; }
    bool operator>(const T& rhs) const { return data > rhs; }

    bool operator<=(const T& rhs) const { return data <= rhs; }
    bool operator>=(const T& rhs) const { return data >= rhs; }

    LittleEndian operator+(const LittleEndian rhs) const { return data + rhs.data; }
    LittleEndian operator-(const LittleEndian rhs) const { return data - rhs.data; }
    LittleEndian operator*(const LittleEndian rhs) const { return data * rhs.data; }
    LittleEndian operator/(const LittleEndian rhs) const { return data / rhs.data; }
    LittleEndian operator%(const LittleEndian rhs) const { return data % rhs.data; }

    LittleEndian operator++(int) { return data++; }
    LittleEndian operator--(int) { return data--; }

    LittleEndian operator&(const LittleEndian rhs) const { return data & rhs.data; }
    LittleEndian operator|(const LittleEndian rhs) const { return data | rhs.data; }
    LittleEndian operator^(const LittleEndian rhs) const { return data ^ rhs.data; }
    LittleEndian operator~() const { return ~data; }

    LittleEndian& operator+=(const LittleEndian rhs) {data = data + rhs.data; return *this; }
    LittleEndian& operator-=(const LittleEndian rhs) {data = data - rhs.data; return *this; }
    LittleEndian& operator*=(const LittleEndian rhs) {data = data * rhs.data; return *this; }
    LittleEndian& operator/=(const LittleEndian rhs) {data = data / rhs.data; return *this; }
    LittleEndian& operator%=(const LittleEndian rhs) {data = data % rhs.data; return *this; }
    LittleEndian& operator&=(const LittleEndian rhs) {data = data & rhs.data; return *this; }
    LittleEndian& operator|=(const LittleEndian rhs) {data = data | rhs.data; return *this; }
    LittleEndian& operator^=(const LittleEndian rhs) {data = data ^ rhs.data; return *this; }
    LittleEndian& operator=(T d) { data = d; return *this; }
} __attribute__((__packed__));
#else
#error "Big endian not yet supported"
#endif

#endif //CROCOS_ENDIAN_H