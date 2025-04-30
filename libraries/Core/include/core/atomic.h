//
// Created by Spencer Martin on 4/29/25.
//

#ifndef CROCOS_ATOMIC_H
#define CROCOS_ATOMIC_H
#include <stddef.h>
#include <core/TypeTraits.h>

const size_t _atomic_global_lock_count = (1 << 4);

#ifdef __GNUC__
enum MemoryOrder : int{
    SEQ_CST = __ATOMIC_SEQ_CST,
    ACQUIRE = __ATOMIC_ACQUIRE,
    RELEASE = __ATOMIC_RELEASE,
    RELAXED = __ATOMIC_RELAXED
};
#else
#error "Compiler atomic intrinsics not supported"
#endif

template <typename T>
constexpr bool _use_intrinsic_atomic_ops = (is_trivially_copyable_v<T>) && ((sizeof(T) == 1) || (sizeof(T) == 2) || (sizeof(T) == 4) || (sizeof(T) == 8));

template<typename T>
inline void atomic_store(T& dest, T val, MemoryOrder mem_order = SEQ_CST){
    //use __atomic_store_n if trivially copiable and right size (can I force alignment on arguments?)
    //use lock-based fallback if not. Use C++ concepts to allow use of an object's internal lock if it has one
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        __atomic_store_n(&dest, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_load( T& src, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_load_n(&src, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

inline void tight_spin(){
#ifdef __x86_64__
    asm volatile("pause");
#endif
}

template<typename T>
inline bool atomic_cmpxchg(T& src, T& expected, T value, bool weak = false,
                          MemoryOrder success_order = SEQ_CST, MemoryOrder failure_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_compare_exchange_n(&src, &expected, value, weak, success_order, failure_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
class Atomic {
    alignas(alignof(T)) T value;
public:
    Atomic(T t){
        store(t);
    }

    Atomic() = default;

    void store(T val, MemoryOrder order = SEQ_CST) {
        atomic_store(value, val, order);
    }

    T load(MemoryOrder order = SEQ_CST) const {
        return atomic_load(value, order);
    }

    bool compare_exchange(T& expected, T desired,
                          MemoryOrder success_order = SEQ_CST,
                          MemoryOrder failure_order = SEQ_CST) {
        if(failure_order > success_order) failure_order = success_order;
        return atomic_cmpxchg(value, expected, desired,
                              false, success_order, failure_order);
    }

    bool compare_exchange_v(T expected, T desired,
                          MemoryOrder success_order = SEQ_CST,
                          MemoryOrder failure_order = SEQ_CST) {
        if(failure_order > success_order) failure_order = success_order;
        return atomic_cmpxchg(value, expected, desired,
                              false, success_order, failure_order);
    }

    Atomic& operator=(T val) {
        store(val);
        return *this;
    }

    operator T() const {
        return load();
    }

    bool operator==(T other) const { return load() == other; }
    bool operator!=(T other) const { return load() != other; }
};

class Spinlock {
private:
    Atomic<bool> locked{false};

public:
    void lock();
    void unlock();
    bool try_lock();
};

template<typename LockType>
class LockGuard {
    LockType& lock;

public:
    explicit LockGuard(LockType& l) : lock(l) {
        lock.lock();
    }

    ~LockGuard() {
        lock.unlock();
    }

    // Non-copyable
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

#endif //CROCOS_ATOMIC_H
