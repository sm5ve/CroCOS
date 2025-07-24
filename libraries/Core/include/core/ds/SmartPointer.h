//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_SMARTPOINTER_H
#define CROCOS_SMARTPOINTER_H

#include "stddef.h"
#include <core/utility.h>

template <typename T>
class UniquePtr {
private:
    T* ptr;
public:
    // Constructor
    explicit UniquePtr(T* p = nullptr) : ptr(p) {}

    // Destructor
    ~UniquePtr() {
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

template <typename T>
class SharedPtr {
private:
    class SharedPtrControlBlock {
    public:
        T* ptr;
        size_t refcount;

        SharedPtrControlBlock(T* val_ptr) : ptr(val_ptr), refcount(1) {}
        ~SharedPtrControlBlock() {
            delete ptr;  // When the last SharedPtr is destroyed, delete the object.
        }
    };
    SharedPtrControlBlock* control_block;

public:
    // Constructor
    explicit SharedPtr(T* ptr = nullptr) {
        if (ptr) {
            control_block = new SharedPtrControlBlock(ptr);
        } else {
            control_block = nullptr;
        }
    }

    // Copy Constructor
    SharedPtr(const SharedPtr& other) : control_block(other.control_block) {
        if (control_block) {
            ++control_block->refcount;  // Increment reference count
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
            // Decrement current reference count and delete if needed
            if (control_block && --control_block->refcount == 0) {
                delete control_block;
            }

            // Move new data
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
            control_block = new SharedPtrControlBlock(new_ptr);
        } else {
            control_block = nullptr;
        }
    }

    // Dereference operator
    T& operator*() const {
        return *(control_block -> ptr);
    }

    T& operator[](size_t index) const {
        return (control_block -> ptr)[index];
    }

    // Arrow operator
    T* operator->() const {
        if(!control_block){
            return nullptr;
        }
        return control_block -> ptr;
    }

    // Get raw pointer
    T* get() const {
        if(!control_block){
            return nullptr;
        }
        return control_block -> ptr;
    }

    // Check if it holds a valid pointer
    operator bool() const {
        return (control_block != nullptr) && (control_block -> ptr != nullptr);
    }
};

// Specialization for arrays
template <typename T>
class SharedPtr<T[]> {
private:
    class SharedPtrControlBlock {
    public:
        T* ptr;
        size_t refcount;

        SharedPtrControlBlock(T* val_ptr) : ptr(val_ptr), refcount(1) {}
        ~SharedPtrControlBlock() {
            delete[] ptr;  // Use delete[] for arrays
        }
    };
    SharedPtrControlBlock* control_block;

public:
    // Constructor
    explicit SharedPtr(T* ptr = nullptr) {
        if (ptr) {
            control_block = new SharedPtrControlBlock(ptr);
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
            control_block = new SharedPtrControlBlock(new_ptr);
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

#endif //CROCOS_SMARTPOINTER_H
