//
// Created by Spencer Martin on 1/6/26.
//
#include <mmio/_Register.h>
#include <stdint.h>

template struct Register<uint8_t>;
template struct Register<uint16_t>;
template struct Register<uint32_t>;
template struct Register<uint64_t>;
template struct Register<int8_t>;
template struct Register<int16_t>;
template struct Register<int32_t>;
template struct Register<int64_t>;