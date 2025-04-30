//
// Created by Spencer Martin on 4/28/25.
//

#ifndef CROCOS_HANDLE_H
#define CROCOS_HANDLE_H

#include <stdint.h>
#include <core/atomic.h>

template<typename Tag>
class Handle {
private:
    struct ControlBlock {
        Atomic<bool> alive;
        Atomic<uint64_t> refCount;
    };

    uint64_t id;
    ControlBlock* ctrl;

    static Atomic<uint64_t> nextID;

public:
    Handle() : id(0), ctrl(nullptr) {}

    static Handle create() {
        Handle h;
        h.id = (nextID.add_fetch(1, RELAXED));
        h.ctrl = new ControlBlock{ true, 1};
        return h;
    }

    Handle(const Handle& other) : id(other.id), ctrl(other.ctrl) {
        if (ctrl) {
            ctrl -> refCount.add_fetch(1, RELAXED);
        }
    }

    Handle& operator=(const Handle& other) {
        if (this != &other) {
            // Decrement current
            if (ctrl && (ctrl -> refCount.sub_fetch(1, RELEASE)) == 0) {
                delete ctrl;
            }

            // Copy from other
            id = other.id;
            ctrl = other.ctrl;
            if (ctrl) {
                ctrl->refCount.add_fetch(1, RELAXED);
            }
        }
        return *this;
    }

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (ctrl && (ctrl->refCount.sub_fetch(1, RELEASE)) == 0) {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                delete ctrl;
            }

            id = other.id;
            ctrl = other.ctrl;
            other.id = 0;
            other.ctrl = nullptr;
        }
        return *this;
    }

    Handle(Handle&& other) noexcept : id(other.id), ctrl(other.ctrl) {
        other.id = 0;
        other.ctrl = nullptr;
    }

    ~Handle() {
        if (ctrl && (ctrl -> refCount.sub_fetch(1, RELEASE)) == 0) {
            delete ctrl;
        }
    }

    bool operator==(const Handle& other) const { return id == other.id; }
    bool operator!=(const Handle& other) const { return id != other.id; }
    bool operator<(const Handle& other) const { return id < other.id; }

    explicit operator bool() const {
        return id != 0 && ctrl && ctrl -> alive;
    }

    uint64_t raw() const { return id; }

    void revoke() {
        if(ctrl) {
            ctrl->alive = false;
        }
    }
};

// Static variable definition
template<typename Tag>
Atomic<uint64_t> Handle<Tag>::nextID = 0;

#endif //CROCOS_HANDLE_H
