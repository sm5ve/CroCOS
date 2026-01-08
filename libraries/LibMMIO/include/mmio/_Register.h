//
// Created by Spencer Martin on 1/6/26.
//

#ifndef CROCOS__REGISTER_H
#define CROCOS__REGISTER_H

#include <core/atomic.h>

#define MMIO_CONSERVATIVE_FENCES

template <typename T>
struct Register {
    volatile T val;
public:
    T read() const {
#ifdef MMIO_CONSERVATIVE_FENCES
        thread_fence(ACQUIRE);
#endif
        T out = val;
        thread_fence(ACQUIRE);
        return out;
    }

    void write(T t) {
        thread_fence(RELEASE);
        val = t;
#ifdef MMIO_CONSERVATIVE_FENCES
        thread_fence(RELEASE);
#endif
    }

    operator T() const { return read(); }
    Register& operator=(T t) { write(t); return *this; }

    //These could surely be done in a smarter way
    Register& operator |=(T t) {
        write(read() | t);
        return *this;
    }

    Register& operator &=(T t) {
        write(read() & t);
        return *this;
    }
} __attribute__((packed));

#endif //CROCOS__REGISTER_H