//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_MM_H
#define CROCOS_MM_H

#include "stddef.h"
#include <core/TypeTraits.h>
#include <arch.h>
#include <core/ds/Vector.h>
#include <core/Flags.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>

// ==================== Allocation Behavior Flags ====================

enum class AllocBehavior : uint32_t {
    BIG_PAGE_ONLY     = 1u << 0,  // Only allocate big (2MiB) pages; never fall back to small pages
    LOCAL_DOMAIN_ONLY = 1u << 1,  // Only allocate from the calling CPU's local pool; never go to NUMA pool
    GRACEFUL_OOM      = 1u << 2,  // Return a short count instead of panicking when memory is exhausted
};
template<> struct is_flags_enum<AllocBehavior> { static constexpr bool value = true; };
using AllocFlags = Flags<AllocBehavior>;

namespace kernel::mm{

    struct MemoryStatistics{
        Vector<size_t> freeBigPageCount;
        Vector<size_t> freeSmallPageCount; //includes sub-pages of big pages
        size_t globalPoolSize;
    };

    namespace PageAllocator{
        // Page count constants used throughout the allocator.
        constexpr size_t smallPagesPerBigPage = arch::bigPageSize / arch::smallPageSize;
        constexpr size_t bigPagesInMaxMemory = arch::maxMemorySupported / arch::bigPageSize;

        // ---- Single-page allocation (returns phys_addr) ----

        phys_addr allocateSmallPage();
        phys_addr allocateSmallPage(numa::DomainID targetDomain);
        phys_addr allocateSmallPage(arch::ProcessorID targetProc);

        phys_addr allocateBigPage();
        phys_addr allocateBigPage(numa::DomainID targetDomain);
        phys_addr allocateBigPage(arch::ProcessorID targetProc);

        // ---- Bulk allocation (callback receives one PageRef per page) ----
        // count is in small pages normally; in big pages when BIG_PAGE_ONLY is set.
        // Returns the number of pages allocated. With default flags this always equals
        // count (panics on OOM). Pass GRACEFUL_OOM to get a short count instead.

        size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, AllocFlags flags = {});
        size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, numa::DomainID targetDomain, AllocFlags flags = {});
        size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, arch::ProcessorID targetProc, AllocFlags flags = {});

        // ---- Single-page free ----

        void freeSmallPage(phys_addr);
        void freeBigPage(phys_addr);

        // ---- Bulk free (pages array is sorted in place) ----

        void freePages(PageRef* pages, size_t count);
    }

    namespace vm {
        enum class PageFaultHandleResult {
            HANDLED_IN_KERNEL,
            DEFERRED,
            UNHANDLED
        };

        enum class PageFaultType {
            READ_FAULT,
            WRITE_FAULT
        };

        class BackingRegion {
        protected:
            char *name;
            PageMappingCacheType cacheType;
        public:
            const char *getName();

            [[nodiscard]]
            virtual size_t getSize() const = 0;

            [[nodiscard]]
            virtual PageFaultHandleResult handlePageFault(virt_addr faulting_addr, virt_addr faulting_ip, PageFaultType) const = 0;
            virtual ~BackingRegion() = default;
            virtual PageMappingCacheType getCacheType();
        };

        class PhysicalBackingRegion : public BackingRegion {
        private:
            enum PageType {
                PRESENT_EXCLUSIVELY_OWNED,  // No refcounting needed, directly stores phys_addr
                PRESENT_SHARED,             // Uses a heap-allocated RefCountedPage*
                LAZY,
                VACANT,
                COPY_ON_WRITE
            };

            struct RefCountedPage {
                phys_addr presentPageAddr;
                uint64_t refCount;
            };

            struct BackingPage {
                PageType type;
                PageSize size;
                union {
                    phys_addr exclusivePageAddr;   // Used for PRESENT_EXCLUSIVELY_OWNED
                    RefCountedPage* sharedPage;    // Used for PRESENT_SHARED & COW
                };

                ~BackingPage();
            };

            Vector<BackingPage> backing;
        };

        class SubPageMMIOBackingRegion : public BackingRegion {
        private:
            phys_memory_range window;
        public:
            explicit SubPageMMIOBackingRegion(phys_memory_range);
        };

        class VirtualAddressZone;

        class RegionMapping {
        private:
            BackingRegion &backingRegion;
            char *name;
            virt_addr base;
            PageMappingPermissions permissions;

            friend VirtualAddressZone;
        public:
            RegionMapping(BackingRegion &, PageMappingPermissions, char *name = (char *) "");

            [[nodiscard]]
            const char *getName();

            [[nodiscard]]
            const virt_addr getBase();
        };

        class VirtualAddressZone {
        private:
            Vector<RegionMapping> mappings;
            //Add augmented AVL tree structure here
        public:
            //Finds somewhere in the virtual address space that can fit our region and maps it.
            //
            virt_addr mapRegion(RegionMapping&&);
            bool mapRegion(RegionMapping&&, virt_addr base);
        };

        class VirtualAddressSpace {
        private:
            Vector<VirtualAddressZone> zones;
        public:
        };
    }
}

#endif //CROCOS_MM_H
