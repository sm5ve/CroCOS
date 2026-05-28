//
// Created by Spencer Martin on 4/26/26.
//

#ifndef CROCOS_VMSUBSTRATE_H
#define CROCOS_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>

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

    // ─── vmsmalloc Phase 3 — init-only NUMA-aware pinned-buffer primitive ───
    //
    // Allocates `divideAndRoundUp(byteSize, smallPageSize)` physical pages
    // on NUMA domain `d`, maps them contiguously inside VMSubstrate's
    // static-buffer region, zero-fills, and returns the base VA. The
    // returned VA is page-aligned and pinned for the kernel's lifetime
    // (no freeing API). Single-threaded init context only — asserts in
    // debug builds; panics on PageAllocator failure or region exhaustion.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);

    // Returns the base VA of CPU `i`'s per-CPU CpuLocal page (sized via
    // kernel::kCpuLocalBytes in cpu_local.h). The page is allocated and
    // mapped during createArena(i) on the arena owner's NUMA domain, and
    // is zero-filled at that time. Pure address arithmetic — safe to call
    // from any context after VMSubstrate::init returns.
    void* cpuLocalPageFor(arch::ProcessorID i);

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
