//
// Created by Spencer Martin on 4/26/26.
//

#ifndef CROCOS_VMSUBSTRATE_H
#define CROCOS_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>

namespace kernel::mm::VMSubstrate {
    bool init();

    // Returns the base virtual address of the arena at the given index.
    virt_addr arenaVirtualBase(size_t index);
    void* allocPage();
    void freePage(void*);

    void ensureTLBEntryFresh(void*);

    void* vmsmalloc(size_t size);
    void vmsfree(void*);

    // Maps a physical MMIO page into the current CPU's arena with cache-disable semantics.
    // paddr must be page-aligned.  The returned virtual address is permanently mapped.
    void* mapMMIOPage(phys_addr paddr);

    template <typename T>
    struct SafePtr {
    private:
        T* ptr;
    public:
        SafePtr(T* p) : ptr(p) {}
        SafePtr(SafePtr& other) : ptr(other.ptr) {}
        SafePtr(const SafePtr& other) : ptr(other.ptr) {}
        SafePtr(SafePtr&& other) noexcept : ptr(other.ptr) {other.ptr=nullptr;}
        T& operator*() const { ensureTLBEntryFresh(ptr); return *ptr; }
        T* operator->() const { ensureTLBEntryFresh(ptr); return ptr; }
        bool operator==(const SafePtr & other) const { return ptr == other.ptr; }
        bool operator!=(const SafePtr & other) const { return ptr != other.ptr; }
        operator bool() const { return ptr != nullptr; }
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        void* raw() const { return ptr; }
    };

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        auto* mem = vmsmalloc(sizeof(T));
        return new (mem) T(forward<Ts>(args)...);
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) {
            obj->~T();
            vmsfree(obj.raw());
        }
    }
}

#endif //CROCOS_VMSUBSTRATE_H
