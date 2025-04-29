//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_SMARTPOINTER_H
#define CROCOS_SMARTPOINTER_H

#include "stddef.h"
#include "../utility.h"

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
            other.ptr = nullptr;
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

//TODO implement WeakPtr and SmartPtr

template <typename T, typename... Args>
UniquePtr<T> make_unique(Args&&... args) {
    return UniquePtr<T>(new T(forward<Args>(args)...));
}

template <typename T, typename... Args>
UniquePtr<T> make_shared(Args&&... args) {
    return SharedPtr<T>(new T(forward<Args>(args)...));
}

#endif //CROCOS_SMARTPOINTER_H
