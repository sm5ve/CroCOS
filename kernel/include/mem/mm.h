//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_MM_H
#define CROCOS_MM_H

#include "stddef.h"
#include <core/TypeTraits.h>
#include <arch.h>
#include <core/ds/Vector.h>
#include <kernel.h>

namespace kernel::mm{
    struct MemoryStatistics{
        Vector<size_t> freeBigPageCount;
        Vector<size_t> freeSmallPageCount; //includes sub-pages of big pages
        size_t globalPoolSize;
    };

    namespace PageAllocator{
        struct page_allocator_range_info{
            phys_memory_range range;
            //The architecture initialization routine MUST reserve adequately sized buffers for each
            //memory range as specified by requestedBufferSizeForRange. The buffers are expected to be
            //zeroed out.
            void* buffer_start;
        };
        // Calculate the maximum page counts
        constexpr size_t smallPagesPerBigPage = arch::bigPageSize / arch::smallPageSize;
        constexpr size_t bigPagesInMaxMemory = arch::maxMemorySupported / arch::bigPageSize;
        void init(Vector<page_allocator_range_info>& regions, size_t processor_count);
        size_t requestedBufferSizeForRange(mm::phys_memory_range range, size_t processor_count);
        void reservePhysicalRange(phys_memory_range);
        phys_addr allocateSmallPage();
        phys_addr allocateBigPage();
        void freeLocalSmallPage(phys_addr);
        void freeLocalBigPage(phys_addr);
        void freeSmallPage(phys_addr);
        void freeBigPage(phys_addr);
        bool allocatePages(size_t requestedCapacityInBytes, Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);
        void freePages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);
        void freeLocalPages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);
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
