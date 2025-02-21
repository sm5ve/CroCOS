//
// Created by Spencer Martin on 2/18/25.
//

#ifndef CROCOS_MATH_H
#define CROCOS_MATH_H

template <typename T>
constexpr T divideAndRoundUp(T numerator, T denominator){
    return (numerator + (denominator - 1)) / denominator;
}

template <typename T>
constexpr T divideAndRoundDown(T numerator, T denominator){
    return numerator / denominator;
}

template <typename T>
constexpr T roundUpToNearestMultiple(T toRound, T divisor){
    return divideAndRoundUp(toRound, divisor) * divisor;
}

template <typename T>
constexpr T roundDownToNearestMultiple(T toRound, T divisor){
    return divideAndRoundDown(toRound, divisor) * divisor;
}

#endif //CROCOS_MATH_H
