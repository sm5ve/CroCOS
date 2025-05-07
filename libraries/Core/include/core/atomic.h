//
// Created by Spencer Martin on 4/29/25.
//

#ifndef CROCOS_ATOMIC_H
#define CROCOS_ATOMIC_H
#include <stddef.h>
#include <core/TypeTraits.h>
#include <core/utility.h>

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
    //use lock-based fallback if not. Use C++ concepts to allow use of an object's internal acquire if it has one
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

template<typename T>
inline T atomic_and_fetch( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_and_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_or_fetch( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_or_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_xor_fetch( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_xor_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_nand_fetch(T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_nand_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_add_fetch(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_add_fetch(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_sub_fetch(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_sub_fetch(&src, val, mem_order);
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

    struct ChangedVal{
        T oldVal;
        T newVal;
    } ;

    template<typename F>
    __attribute__((always_inline)) ChangedVal update_and_get(F&& transform) {
        T old_val, new_val;
        do {
            old_val = load(ACQUIRE);
            new_val = transform(old_val);
        } while (!compare_exchange(old_val, new_val, RELEASE, RELAXED));
        return {old_val, new_val};
    }

    template<typename F, typename G>
    __attribute__((always_inline)) ChangedVal update_and_get_when(G&& condition, F&& transform) {
        T old_val, new_val;
        do {
            do{
                old_val = load(ACQUIRE);
            }while(!condition(old_val));
            new_val = transform(old_val);
        } while (!compare_exchange(old_val, new_val, RELEASE, RELAXED));
        return {old_val, new_val};
    }

    bool operator==(T other) const { return load() == other; }
    bool operator!=(T other) const { return load() != other; }

    T operator &=(T mask) requires is_integral_v<T> {
        return atomic_and_fetch(value, mask);
    }

    T operator |=(T mask) requires is_integral_v<T> {
        return atomic_or_fetch(value, mask);
    }

    T operator +=(T val) requires is_integral_v<T> {
        return atomic_add_fetch(value, val);
    }

    T operator -=(T val) requires is_integral_v<T> {
        return atomic_sub_fetch(value, val);
    }

    T add_fetch(T val, MemoryOrder mem_order = SEQ_CST){
        return atomic_and_fetch(value, val, mem_order);
    }

    T sub_fetch(T val, MemoryOrder mem_order = SEQ_CST){
        return atomic_sub_fetch(value, val, mem_order);
    }
};

class Spinlock {
private:
    Atomic<bool> locked{false};

public:
    void acquire();
    bool try_acquire();
    void release();
};

class RWSpinlock{
private:
    Atomic<uint64_t> lockstate{0};
public:
    void acquire_reader();
    bool try_acquire_reader();
    void acquire_writer();
    bool try_acquire_writer();
    void release_reader();
    void release_writer();
    bool writer_lock_taken();
    bool reader_lock_taken();
};

template<typename LockType>
class LockGuard {
    LockType& lock;

public:
    explicit LockGuard(LockType& l) : lock(l) {
        lock.acquire();
    }

    ~LockGuard() {
        lock.release();
    }

    // Non-copyable
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

class WriterLockGuard {
    RWSpinlock& lock;

public:
    explicit WriterLockGuard(RWSpinlock& l) : lock(l) {
        lock.acquire_writer();
    }

    ~WriterLockGuard() {
        lock.release_writer();
    }

    // Non-copyable
    WriterLockGuard(const WriterLockGuard&) = delete;
    WriterLockGuard& operator=(const WriterLockGuard&) = delete;
};

class ReaderLockGuard {
    RWSpinlock& lock;

public:
    explicit ReaderLockGuard(RWSpinlock& l) : lock(l) {
        lock.acquire_reader();
    }

    ~ReaderLockGuard() {
        lock.release_reader();
    }

    // Non-copyable
    ReaderLockGuard(const ReaderLockGuard&) = delete;
    ReaderLockGuard& operator=(const ReaderLockGuard&) = delete;
};

#endif //CROCOS_ATOMIC_H
