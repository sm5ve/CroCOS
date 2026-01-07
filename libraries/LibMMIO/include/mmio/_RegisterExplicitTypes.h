//
// Created by Spencer Martin on 1/7/26.
//

#ifndef CROCOS__REGISTEREXPLICITTYPES_H
#define CROCOS__REGISTEREXPLICITTYPES_H

#ifndef CROCOS_REGISTER_H
#error "Do not include this file directly. Include Register.h instead."
#endif

extern template struct Register<uint8_t>;
extern template struct Register<uint16_t>;
extern template struct Register<uint32_t>;
extern template struct Register<uint64_t>;
extern template struct Register<int8_t>;
extern template struct Register<int16_t>;
extern template struct Register<int32_t>;
extern template struct Register<int64_t>;

#endif //CROCOS__REGISTEREXPLICITTYPES_H