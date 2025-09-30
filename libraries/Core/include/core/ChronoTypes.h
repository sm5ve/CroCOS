//
// Created by Spencer Martin on 9/9/25.
//

#ifndef CROCOS_CHRONOTYPES_H
#define CROCOS_CHRONOTYPES_H

enum class TimeUnit{
    NANOSECONDS = 0,
    MICROSECONDS = 1,
    MILLISECONDS = 2,
    SECONDS = 3
}

class Duration{
    size_t amount;
    TimeUnit unit;
public:
    Duration(size_t amount, TimeUnit unit);
    Duration operator+(const Duration& other) const;
    Duration operator*(const size_t amount) const;
    Duration operator/(const size_t amount) const;
    Duration& operator+=(const Duration& other);
    Duration& operator*=(const size_t amount);
    Duration& operator/=(const size_t amount);
    int operator<=> (const Duration& other) const;
}

#endif //CROCOS_CHRONOTYPES_H