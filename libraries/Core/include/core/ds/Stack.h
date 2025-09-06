//
// Created by Spencer Martin on 8/4/25.
//

#ifndef STACK_H
#define STACK_H

#include <core/ds/Vector.h>
#include <core/utility.h>
#include <assert.h>
#include <stddef.h>

template <typename T>
using Stack = Vector<T>;

template <typename T, size_t N>
//TODO add bounds checks???
class StaticStack{
private:
    T data[N];
    size_t stackPointer;
public:
    StaticStack() : stackPointer(0) {}

    void push(T t){
		assert(stackPointer < N, "Stack is full");
        data[stackPointer++] = t;
    }

    T pop(){
        return data[--stackPointer];
    }

    size_t size() const{
        return stackPointer;
    }

    bool empty(){
        return stackPointer == 0;
    }

    T* top(){
        return &data[stackPointer - 1];
    }

	template <UnsignedIntegral Index>
    T& operator[](Index index) {
        assert(index < stackPointer, "Index out of bounds");
        return data[index];
    }

    template <UnsignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < stackPointer, "Index out of bounds");
        return data[index];
    }

    template <SignedIntegral Index>
    T& operator[](Index index) {
        assert(index < (Index)stackPointer, "Index out of bounds");
        assert((Index)stackPointer + index >= 0, "Index out of bounds");
        if (index < 0)
            return data[stackPointer + index];
        return data[index];
    }

    template <SignedIntegral Index>
    const T& operator[](Index index) const {
        assert(index < (Index)stackPointer, "Index out of bounds");
        assert((Index)stackPointer + index >= 0, "Index out of bounds");
        if (index < 0)
            return data[stackPointer + index];
        return data[index];
    }

	T* begin(){
		return data;
	}

	T* end(){
		return data + stackPointer;
	}
};

template <typename T, typename S>
concept IsStack = requires(T t, S s){
    s.push(t);
    {s.pop()} -> convertible_to<T>;
    {s.size()} -> convertible_to<size_t>;
    {s.empty()} -> convertible_to<bool>;
    {s[0]} -> convertible_to<T&>;
    {s[-1]} -> convertible_to<T&>;
    {s.top()} -> convertible_to<T*>;
};

static_assert(IsStack<int, Stack<int>>);
static_assert(IsStack<int, StaticStack<int, 10>>);

#endif //STACK_H
