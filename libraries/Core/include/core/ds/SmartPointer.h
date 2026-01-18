//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_SMARTPOINTER_H
#define CROCOS_SMARTPOINTER_H

#include <core/atomic.h>

#include "stddef.h"
#include <core/utility.h>
#include <core/TypeTraits.h>

template <typename T>
class UniquePtr {
private:
    T* ptr;
public:
    // Constructor
    explicit UniquePtr(T* p = nullptr) : ptr(p) {}

    // Destructor
    ~UniquePtr() {
        if (ptr)
            delete ptr;
    }

    // Deleted Copy Constructor and Copy Assignment (unique ownership)
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    // Move Constructor
    UniquePtr(UniquePtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    // Move Assignment
    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            delete ptr;
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    // Pointer-like behavior
    T& operator*() const {
        return *ptr;
    }

    T* operator->() const {
        return ptr;
    }

    // Check if non-null
    explicit operator bool() const {
        return ptr != nullptr;
    }

    // Release ownership (no deletion)
    T* release() {
        T* temp = ptr;
        ptr = nullptr;
        return temp;
    }

    // Reset the pointer (deletes current)
    void reset(T* new_ptr = nullptr) {
        delete ptr;
        ptr = new_ptr;
    }

    // Get the raw pointer
    T* get() const {
        return ptr;
    }
};

// Specialization for arrays
template <typename T>
class UniquePtr<T[]> {
private:
    T* ptr;

public:
    // Constructor
    explicit UniquePtr(T* p = nullptr) : ptr(p) {}

    // Destructor
    ~UniquePtr() {
        delete[] ptr;  // Use delete[] for arrays
    }

    // Deleted Copy Constructor and Copy Assignment (unique ownership)
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    // Move Constructor
    UniquePtr(UniquePtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    // Move Assignment
    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            delete[] ptr;
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    // Array access operator
    T& operator[](size_t index) const {
        return ptr[index];
    }

    // Check if non-null
    explicit operator bool() const {
        return ptr != nullptr;
    }

    // Release ownership (no deletion)
    T* release() {
        T* temp = ptr;
        ptr = nullptr;
        return temp;
    }

    // Reset the pointer (deletes current)
    void reset(T* new_ptr = nullptr) {
        delete[] ptr;
        ptr = new_ptr;
    }

    // Get the raw pointer
    T* get() const {
        return ptr;
    }

    // Static factory method for arrays
    static UniquePtr<T[]> make(size_t count) {
        return UniquePtr<T[]>(new T[count]);
    }
};

class SharedPtrControlBlock {
public:
    void* original_ptr;
    void (*deleter)(void*);
    Atomic<size_t> refcount;

    template<typename U>
    SharedPtrControlBlock(U* val_ptr) : original_ptr(val_ptr), refcount(1) {
        deleter = [](void* p) { delete static_cast<U*>(p); };
    }
    ~SharedPtrControlBlock() {
        if (original_ptr && deleter) {
            deleter(original_ptr);
        }
    }
};

template <typename T>
class SharedPtr;

template<typename S, typename U>
SharedPtr<S> static_pointer_cast(const SharedPtr<U>& ptr);

template<typename S, typename U>
SharedPtr<S> static_pointer_cast(SharedPtr<U>& ptr);

template <typename T>
class SharedPtr {
private:
    SharedPtrControlBlock* control_block;
    T* typed_ptr;

    // Internal constructor for sharing control blocks with different types
    template<typename U>
    SharedPtr(const SharedPtr<U>& other, T* new_typed_ptr)
        : control_block(other.control_block), typed_ptr(new_typed_ptr) {
        if (control_block) {
            ++control_block->refcount;
        }
    }

    template<typename S, typename U>
    friend SharedPtr<S> static_pointer_cast(const SharedPtr<U>& ptr);

    template<typename S, typename U>
    friend SharedPtr<S> static_pointer_cast(SharedPtr<U>& ptr);

public:
    constexpr static SharedPtr null() {
        return SharedPtr();
    }

    // Constructor
    explicit SharedPtr(T* ptr = nullptr) {
        if (ptr) {
            control_block = new SharedPtrControlBlock(ptr);
            typed_ptr = ptr;
        } else {
            control_block = nullptr;
            typed_ptr = nullptr;
        }
    }

    // Copy Constructor
    SharedPtr(const SharedPtr& other) : control_block(other.control_block), typed_ptr(other.typed_ptr) {
        if (control_block) {
            ++control_block->refcount;  // Increment reference count
        }
    }

    // Template copy constructor for compatible types
    template<typename U>
    SharedPtr(const SharedPtr<U>& other) requires StaticCastable<U*, T*>
        : control_block(other.control_block) {
        if (control_block) {
            typed_ptr = static_cast<T*>(other.typed_ptr);
            ++control_block->refcount;
        } else {
            typed_ptr = nullptr;
        }
    }

    // Copy Assignment
    SharedPtr& operator=(const SharedPtr& other) {
        if (this != &other) {
            // Decrement current reference count and delete if needed
            if (control_block && --control_block->refcount == 0) {
                delete control_block;
            }

            control_block = other.control_block;
            typed_ptr = other.typed_ptr;
            if (control_block) {
                ++control_block->refcount;
            }
        }
        return *this;
    }

    // Move Constructor
    SharedPtr(SharedPtr&& other) noexcept : control_block(other.control_block), typed_ptr(other.typed_ptr) {
        other.control_block = nullptr;
        other.typed_ptr = nullptr;
    }

    // Move Assignment
    SharedPtr& operator=(SharedPtr&& other) noexcept {
        if (this != &other) {
            // Decrement current reference count and delete if needed
            if (control_block && --control_block->refcount == 0) {
                delete control_block;
            }

            // Move new data
            control_block = other.control_block;
            typed_ptr = other.typed_ptr;
            other.control_block = nullptr;
            other.typed_ptr = nullptr;
        }
        return *this;
    }

    // Destructor
    ~SharedPtr() {
        if (control_block && --control_block->refcount == 0) {
            delete control_block;
        }
    }

    // Reset function
    void reset(T* new_ptr = nullptr) {
        if (control_block && --control_block->refcount == 0) {
            delete control_block;
        }
        if (new_ptr) {
            control_block = new SharedPtrControlBlock(new_ptr);
            typed_ptr = new_ptr;
        } else {
            control_block = nullptr;
            typed_ptr = nullptr;
        }
    }

    // Dereference operator
    T& operator*() const {
        return *typed_ptr;
    }

    T& operator[](size_t index) const {
        return typed_ptr[index];
    }

    // Arrow operator
    T* operator->() const {
        return typed_ptr;
    }

    // Get raw pointer
    T* get() const {
        return typed_ptr;
    }

    // Check if it holds a valid pointer
    operator bool() const {
        return typed_ptr != nullptr;
    }

    bool operator==(const SharedPtr<T>& other) const {
        return typed_ptr == other.typed_ptr && control_block == other.control_block;
    }

    // Friend declarations for casting functions
    template<typename U> friend class SharedPtr;
    template<typename U, typename V> friend SharedPtr<U> static_pointer_cast(const SharedPtr<V>& ptr);
    template<typename U, typename V> friend SharedPtr<U> crocos_dynamic_cast(const SharedPtr<V>& ptr);
};

// Casting functions
template<typename S, typename U>
SharedPtr<S> static_pointer_cast(const SharedPtr<U>& ptr) {
    if (!ptr) {
        return SharedPtr<S>();
    }
    S* new_ptr = static_cast<S* const>(ptr.typed_ptr);
    return SharedPtr<S>(ptr, new_ptr);
}

template<typename S, typename U>
SharedPtr<S> static_pointer_cast(SharedPtr<U>& ptr) {
    if (!ptr) {
        return SharedPtr<S>();
    }
    S* new_ptr = static_cast<S*>(ptr.typed_ptr);
    return SharedPtr<S>(ptr, new_ptr);
}

// Specialization for arrays
template <typename T>
class SharedPtr<T[]> {
private:
    class SharedPtrArrayControlBlock {
    public:
        T* ptr;
        size_t refcount;

        SharedPtrArrayControlBlock(T* val_ptr) : ptr(val_ptr), refcount(1) {}
        ~SharedPtrArrayControlBlock() {
            delete[] ptr;  // Use delete[] for arrays
        }
    };
    SharedPtrArrayControlBlock* control_block;

public:
    // Constructor
    explicit SharedPtr(T* ptr = nullptr) {
        if (ptr) {
            control_block = new SharedPtrArrayControlBlock(ptr);
        } else {
            control_block = nullptr;
        }
    }

    // Copy Constructor
    SharedPtr(const SharedPtr& other) : control_block(other.control_block) {
        if (control_block) {
            ++control_block->refcount;
        }
    }

    // Copy Assignment
    SharedPtr& operator=(const SharedPtr& other) {
        if (this != &other) {
            if (control_block && --control_block->refcount == 0) {
                delete control_block;
            }

            control_block = other.control_block;
            if (control_block) {
                ++control_block->refcount;
            }
        }
        return *this;
    }

    // Move Constructor
    SharedPtr(SharedPtr&& other) noexcept : control_block(other.control_block) {
        other.control_block = nullptr;
    }

    // Move Assignment
    SharedPtr& operator=(SharedPtr&& other) noexcept {
        if (this != &other) {
            if (control_block && --control_block->refcount == 0) {
                delete control_block;
            }

            control_block = other.control_block;
            other.control_block = nullptr;
        }
        return *this;
    }

    // Destructor
    ~SharedPtr() {
        if (control_block && --control_block->refcount == 0) {
            delete control_block;
        }
    }

    // Reset function
    void reset(T* new_ptr = nullptr) {
        if (control_block && --control_block->refcount == 0) {
            delete control_block;
        }
        if (new_ptr) {
            control_block = new SharedPtrArrayControlBlock(new_ptr);
        } else {
            control_block = nullptr;
        }
    }

    // Array access operator
    T& operator[](size_t index) const {
        return (control_block->ptr)[index];
    }

    // Get raw pointer
    T* get() const {
        if (!control_block) {
            return nullptr;
        }
        return control_block->ptr;
    }

    // Check if it holds a valid pointer
    operator bool() const {
        return (control_block != nullptr) && (control_block->ptr != nullptr);
    }

    // Static factory method for arrays
    static SharedPtr<T[]> make(size_t count) {
        return SharedPtr<T[]>(new T[count]);
    }
};

//TODO implement WeakPtr

template <typename T, typename... Args>
UniquePtr<T> make_unique(Args&&... args) {
    return UniquePtr<T>(new T(forward<Args>(args)...));
}

template <typename T, typename... Args>
SharedPtr<T> make_shared(Args&&... args) {
    return SharedPtr<T>(new T(forward<Args>(args)...));
}

template <typename T>
SharedPtr<T[]> make_shared_array(size_t count) {
    return SharedPtr<T[]>::make(count);
}

template <typename T>
UniquePtr<T[]> make_unique_array(size_t count) {
    return UniquePtr<T[]>::make(count);
}

template<typename K>
struct DefaultHasher;

template<typename T>
struct DefaultHasher<SharedPtr<T>> {
    size_t operator()(const SharedPtr<T>& key) const {
        return reinterpret_cast<size_t>(key.get()) >> 3;
    }
};

template<typename T>
struct DefaultHasher<SharedPtr<T[]>> {
    size_t operator()(const SharedPtr<T>& key) const {
        return reinterpret_cast<size_t>(key.get()) >> 3;
    }
};

#include <core/internal/SharedPtrDynamicCast.h>

#endif //CROCOS_SMARTPOINTER_H