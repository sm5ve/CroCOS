//
// Created by Spencer Martin on 4/30/25.
//

#ifndef CROCOS_RINGBUFFER_H
#define CROCOS_RINGBUFFER_H

#include <stddef.h>
#include <core/atomic.h>
#include <core/TypeTraits.h>

template<typename T>
class RingBuffer{
public:
    class Iterator{
        RingBuffer* buf;
        size_t index;
        bool is_end_sentinel;

    public:
        Iterator(RingBuffer* b, size_t i, bool sentinel = false)
                : buf(b), index(i), is_end_sentinel(sentinel) {}

        T& operator*() { return buf->buffer[index]; }
        Iterator& operator++() {
            index = (index + 1) % buf->capacity;
            return *this;
        }
        bool operator!=(const Iterator& other) const {
            if(other.buf != buf) return true;
            auto thisIndex = is_end_sentinel ? buf->writtenHead.load(ACQUIRE) : index;
            auto otherIndex = other.is_end_sentinel ? buf->writtenHead.load(ACQUIRE) : other.index;
            return thisIndex != otherIndex;
        }
    };
private:
    T* buffer;
    const size_t capacity;
    Atomic<size_t> readHead, writeHead, writtenHead;
    friend Iterator;
public:
    RingBuffer(size_t s) : capacity(s){
        readHead = 0;
        writtenHead = 0;
        writeHead = 0;
        buffer = static_cast<T*>(operator new(sizeof(T) * capacity));
    }

    size_t size(){
        return (capacity + writtenHead.load(ACQUIRE) - readHead.load(ACQUIRE)) % capacity;
    }

    bool enqueue(T&& val){
        while(true){
            size_t oldHead = writeHead.load(ACQUIRE);
            size_t newWriteHead = (oldHead + 1) % capacity;
            //If enqueueing the value will fill the buffer, return false and abort
            if(newWriteHead == readHead.load(ACQUIRE)) return false;
            if(writeHead.compare_exchange(oldHead, newWriteHead, RELEASE, ACQUIRE)){
                new (&buffer[oldHead]) T(forward<T>(val));
                while(!writtenHead.compare_exchange(oldHead, newWriteHead, RELEASE, ACQUIRE)){tight_spin();}
                return true;
            }
        }
    }

    bool try_advance_read_head(){
        size_t oldHead = readHead.load(ACQUIRE);
        if (oldHead == writtenHead.load(ACQUIRE)) return false;
        size_t newHead = (oldHead + 1) % capacity;
        if (readHead.compare_exchange(oldHead, newHead, RELEASE, ACQUIRE)) {
            return true;
        }
        else{
            return false;
        }
    }

    bool dequeue(T& out){
        while(true){
            size_t oldHead = readHead.load(ACQUIRE);
            if(oldHead == writtenHead.load(ACQUIRE)) return false;
            size_t newHead = (oldHead + 1) % capacity;
            if(readHead.compare_exchange(oldHead, newHead, RELEASE, ACQUIRE)){
                out = move(buffer[oldHead]);
                buffer[oldHead].~T();
                return true;
            }
        }
    }

    ~RingBuffer() {
        size_t r = readHead.load(RELAXED);
        size_t w = writtenHead.load(RELAXED);
        while (r != w) {
            buffer[r].~T();
            r = (r + 1) % capacity;
        }
        operator delete(buffer);
    }

    Iterator begin() { return Iterator(this, readHead.load(ACQUIRE)); }
    Iterator end()   { return Iterator(this, 0, true); }

    void advance_read_head_until(FunctionRef<bool(T&)> condition){
        while(true){
            size_t oldRHead = readHead.load(ACQUIRE);
            if(oldRHead == writtenHead.load(ACQUIRE)) break;
            if(condition(buffer[oldRHead])) break;
            size_t newRHead = (oldRHead + 1) % capacity;
            readHead.compare_exchange(oldRHead, newRHead, RELEASE, ACQUIRE);
        }
    }
};

#endif //CROCOS_RINGBUFFER_H
