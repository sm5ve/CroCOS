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
    ACQ_REL = __ATOMIC_ACQ_REL,
    ACQUIRE = __ATOMIC_ACQUIRE,
    RELEASE = __ATOMIC_RELEASE,
    RELAXED = __ATOMIC_RELAXED
};
#else
#error "Compiler atomic intrinsics not supported"
#endif

template <typename T>
constexpr bool _use_intrinsic_atomic_ops = (is_trivially_copyable_v<T>) && ((sizeof(T) == 1) || (sizeof(T) == 2) || (sizeof(T) == 4) || (sizeof(T) == 8));

// Storage type for Atomic<T>:
//   enum     → underlying integer type  (existing behaviour)
//   pointer  → uintptr_t               (avoids volatile-pointer-to-T vs volatile-pointer issues)
//   other    → T itself                (existing behaviour)
template<typename T>
using _atomic_storage_t = conditional_t<
    is_enum_v<T>,    underlying_type_t<T>,
    conditional_t<
        is_pointer_v<T>, uintptr_t,
        T
    >
>;

template<typename T>
inline void atomic_store(T& dest, T val, MemoryOrder mem_order = SEQ_CST){
    //use __atomic_store_n if trivially copiable and right size (can I force alignment on arguments?)
    //use lock-based fallback if not. Use C++ concepts to allow use of an object's internal acquire if it has one
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        __atomic_store_n(&dest, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline void atomic_store(volatile T& dest, T val, MemoryOrder mem_order = SEQ_CST){
    //use __atomic_store_n if trivially copiable and right size (can I force alignment on arguments?)
    //use lock-based fallback if not. Use C++ concepts to allow use of an object's internal acquire if it has one
    if constexpr(_use_intrinsic_atomic_ops<T>){
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
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_load_n(&src, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_load(volatile T& src, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
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
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_and_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_and_fetch(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
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
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_or_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_or_fetch( volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
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
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_xor_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_xor_fetch( volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
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
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_nand_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_nand_fetch(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_nand_fetch(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_and( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_and(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_and(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_and(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_or( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_or(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_or(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_or(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_xor( T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_xor(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_xor(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_xor(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_nand(T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_nand(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_nand(volatile T& src, T mask, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_nand(&src, mask, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_add_fetch(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_add_fetch(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_add_fetch(volatile T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_add_fetch(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_add(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_add(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_add(volatile T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_add(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_sub_fetch(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_sub_fetch(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_sub_fetch(volatile T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_sub_fetch(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_sub(T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_sub(&src, val, mem_order);
#endif
    }
    else{
        static_assert(_use_intrinsic_atomic_ops<T>, "Unimplemented");
    }
}

template<typename T>
inline T atomic_fetch_sub(volatile T& src, T val, MemoryOrder mem_order = SEQ_CST){
    if constexpr(_use_intrinsic_atomic_ops<T>){
#ifdef __GNUC__
        return __atomic_fetch_sub(&src, val, mem_order);
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
inline bool atomic_cmpxchg(volatile T& src, T& expected, T value, bool weak = false,
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

inline void thread_fence(MemoryOrder order = SEQ_CST){
#ifdef __GNUC__
    __atomic_thread_fence(order);
#endif
}

template<typename T>
class Atomic {
    using S = _atomic_storage_t<T>;
    alignas(alignof(S)) volatile S value;
public:
    Atomic(T t){
        store(t);
    }

    Atomic() = default;

    void store(T val, MemoryOrder order = SEQ_CST) {
        if constexpr (is_pointer_v<T>)
            atomic_store(value, reinterpret_cast<S>(val), order);
        else
            atomic_store(value, static_cast<S>(val), order);
    }

    [[nodiscard]] T load(MemoryOrder order = SEQ_CST) const {
        if constexpr (is_pointer_v<T>)
            return reinterpret_cast<T>(atomic_load(value, order));
        else
            return static_cast<T>(atomic_load(value, order));
    }

    bool compare_exchange(T& expected, T desired,
                          MemoryOrder success_order = SEQ_CST,
                          MemoryOrder failure_order = SEQ_CST) {
        if(failure_order > success_order) failure_order = success_order;
        if constexpr (is_pointer_v<T>) {
            S exp_storage = reinterpret_cast<S>(expected);
            bool result = atomic_cmpxchg(value, exp_storage, reinterpret_cast<S>(desired),
                                         false, success_order, failure_order);
            if (!result) expected = reinterpret_cast<T>(exp_storage);
            return result;
        } else {
            return atomic_cmpxchg(value, static_cast<S&>(expected), static_cast<S>(desired),
                                  false, success_order, failure_order);
        }
    }

    bool compare_exchange_weak(T& expected, T desired,
                          MemoryOrder success_order = SEQ_CST,
                          MemoryOrder failure_order = SEQ_CST) {
        if(failure_order > success_order) failure_order = success_order;
        if constexpr (is_pointer_v<T>) {
            S exp_storage = reinterpret_cast<S>(expected);
            bool result = atomic_cmpxchg(value, exp_storage, reinterpret_cast<S>(desired),
                                         true, success_order, failure_order);
            if (!result) expected = reinterpret_cast<T>(exp_storage);
            return result;
        } else {
            return atomic_cmpxchg(value, static_cast<S&>(expected), static_cast<S>(desired),
                                  true, success_order, failure_order);
        }
    }

    bool compare_exchange_v(T expected, T desired,
                          MemoryOrder success_order = SEQ_CST,
                          MemoryOrder failure_order = SEQ_CST) {
        if(failure_order > success_order) failure_order = success_order;
        if constexpr (is_pointer_v<T>) {
            S exp_storage = reinterpret_cast<S>(expected);
            return atomic_cmpxchg(value, exp_storage, reinterpret_cast<S>(desired),
                                  false, success_order, failure_order);
        } else {
            return atomic_cmpxchg(value, static_cast<S&>(expected), static_cast<S>(desired),
                                  false, success_order, failure_order);
        }
    }

    Atomic& operator=(T val) {
        store(val);
        return *this;
    }

    [[nodiscard]] operator T() const {
        return load();
    }

    struct ChangedVal{
        T oldVal;
        T newVal;
    };

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

    T fetch_and(T mask, MemoryOrder order = SEQ_CST) requires is_integral_v<T> {
        return atomic_fetch_and(value, mask, order);
    }

    T fetch_or(T mask, MemoryOrder order = SEQ_CST) requires is_integral_v<T> {
        return atomic_fetch_or(value, mask, order);
    }

    T fetch_xor(T mask, MemoryOrder order = SEQ_CST) requires is_integral_v<T> {
        return atomic_fetch_xor(value, mask, order);
    }

    T exchange(T desired, MemoryOrder order = SEQ_CST) {
        if constexpr (is_pointer_v<T>)
            return reinterpret_cast<T>(__atomic_exchange_n(&value, reinterpret_cast<S>(desired), order));
        else
            return static_cast<T>(__atomic_exchange_n(&value, static_cast<S>(desired), order));
    }

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

    T add_fetch(T val, MemoryOrder mem_order = SEQ_CST) requires is_integral_v<T>{
        return atomic_add_fetch(value, val, mem_order);
    }

    T fetch_add(T val, MemoryOrder mem_order = SEQ_CST) requires is_integral_v<T>{
        return atomic_fetch_add(value, val, mem_order);
    }

    T sub_fetch(T val, MemoryOrder mem_order = SEQ_CST) requires is_integral_v<T>{
        return atomic_sub_fetch(value, val, mem_order);
    }

    T fetch_sub(T val, MemoryOrder mem_order = SEQ_CST) requires is_integral_v<T>{
        return atomic_fetch_sub(value, val, mem_order);
    }

    T operator++(int) requires is_integral_v<T>{  // Post-increment: return old value
        return atomic_fetch_add<T>(value, 1);
    }

    T operator--(int) requires is_integral_v<T>{  // Post-decrement: return old value
        return atomic_fetch_sub<T>(value, 1);
    }

    T operator++() requires is_integral_v<T>{  // Pre-increment: return new value
        return add_fetch(1);
    }

    T operator--() requires is_integral_v<T>{  // Pre-decrement: return new value
        return sub_fetch(1);
    }
};

class Spinlock {
private:
    Atomic<bool> locked{false};
    static const size_t activeMeta = 1ul << 63;
    Atomic<size_t> metadata{0};

public:
    void acquire();
    bool try_acquire();
    void release();
    [[nodiscard]] bool lock_taken() const;
};

class RWSpinlock{
private:
    Atomic<uint64_t> lockstate{0};
    static const size_t activeMeta = 1ul << 63;
    Atomic<size_t> metadata{0};
public:
    void acquire_reader();
    bool try_acquire_reader();
    void acquire_writer();
    bool try_acquire_writer();
    void release_reader();
    void release_writer();
    [[nodiscard]] bool writer_lock_taken() const;
    [[nodiscard]] bool reader_lock_taken() const;
};

template<typename LockType>
class LockGuard {
    LockType& lock;
    bool manuallyUnlocked = false;

public:
    explicit LockGuard(LockType& l) : lock(l) {
        lock.acquire();
    }

    ~LockGuard() {
        if (!manuallyUnlocked)
            lock.release();
    }

    void unlock() {
        lock.release();
        manuallyUnlocked = true;
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
