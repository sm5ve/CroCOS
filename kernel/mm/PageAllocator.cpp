//
// Created by Spencer Martin on 2/12/25.
//

#include <mm.h>
#include <arch/hal.h>
#include <lib/ds/Vector.h>
#include <kernel.h>
#include <lib/math.h>

#define ALLOCATOR_DEBUG

namespace kernel::mm::PageAllocator{
    //Determine the required types to fit the numbers we'll be dealing with
    //"Absolute" indices - can be derived from an address directly (up to a constant offset)

    using SmallIndexRawType = SmallestUInt_t<RequiredBits(smallPagesPerBigPage)>;
    using BigIndexRawType = SmallestUInt_t<RequiredBits(bigPagesInMaxMemory)>;

    enum class SmallPageIndex : SmallIndexRawType {};
    enum class BigPageIndex : BigIndexRawType {};

    // "Relative" indices - specifically used to point into page pools
    enum class SmallPoolIndex : SmallIndexRawType {};
    enum class BigPoolIndex : BigIndexRawType {};

    // Analogous concepts to the above, but sized to support indicating "full" pools
    using SmallPageBufferOffset = SmallestUInt_t<RequiredBits(smallPagesPerBigPage + 1)>;
    using BigPageBufferOffset = SmallestUInt_t<RequiredBits(bigPagesInMaxMemory + 1)>;

    const SmallPageBufferOffset SMALL_POOL_FULL{smallPagesPerBigPage};

    enum BulkAllocationPolicy{
        USE_UP_SMALL,
        PREFER_BIG
    };

    //We *don't* return a SmallPoolIndex here because
    constexpr size_t getIndexIntoSmallPagePool(BigPoolIndex bigPage, SmallPoolIndex smallPage){
        //Make sure there's no funny business going on with sign extensions
        return ((size_t)bigPage & (bigPageSize - 1)) * smallPagesPerBigPage + ((size_t)smallPage & (smallPageSize - 1));
    }

    constexpr BigPageIndex getBigPageIndexFromAddress(phys_addr addr, phys_memory_range range){
        size_t base_index = (size_t)(range.start.value / bigPageSize);
        size_t abs_index = (size_t)(addr.value / bigPageSize);
        return (BigPageIndex)(abs_index - base_index);
    }

    constexpr phys_addr getBigPageAddress(BigPageIndex bigPage, phys_memory_range range){
        return phys_addr((uint64_t)bigPage * bigPageSize
                         + roundDownToNearestMultiple(range.start.value, bigPageSize));
    }

    constexpr phys_addr getSmallPageAddress(BigPageIndex bigPage, SmallPageIndex smallPage, phys_memory_range range){
        return phys_addr((uint64_t)bigPage * bigPageSize + (uint64_t)smallPage * smallPageSize
                         + roundDownToNearestMultiple(range.start.value, bigPageSize));
    }

    constexpr SmallPageIndex getSmallPageIndexFromAddress(phys_addr addr){
        return (SmallPageIndex)((addr.value / smallPageSize) % smallPagesPerBigPage);
    }

    struct LocalPoolInfo {
        size_t local_pool_size;
        size_t free_small_pages_in_partial_allocs;
        BigPageBufferOffset bottom_of_free_pool;
        BigPageBufferOffset bottom_of_used_pool;
        [[maybe_unused]]
        uint8_t padding[kernel::hal::CACHE_LINE_SIZE - 2 * sizeof(size_t)
                                                     - 2 * sizeof(BigPageBufferOffset)];
    } __attribute__((aligned(kernel::hal::CACHE_LINE_SIZE)));

    struct AlignedBigPageBufferOffset {
        BigPageBufferOffset value;
        uint8_t padding[kernel::hal::CACHE_LINE_SIZE - sizeof(BigPageBufferOffset)];
    } __attribute__((aligned(kernel::hal::CACHE_LINE_SIZE)));

    using BufferId = SmallestUInt_t<RequiredBits(hal::MAX_PROCESSOR_COUNT) + 1>;
    const BufferId GLOBAL_POOL = 0;

    constexpr BufferId fromProcID(kernel::hal::ProcessorID id){
        return id + 1;
    }

    struct BigPageFreeMapping{
        BigPoolIndex index;
        BufferId bufferId;
    };

    //locks for various parts of the allocator
    hal::spinlock_t global_lock;
    hal::spinlock_t* local_locks;

    //Methods in this struct do NOT acquire locks - this is to allow the public-facing page allocation methods
    //to batch their calls and avoid needlessly releasing/reacquiring locks, inducing unnecessary bus stalls
    struct ContiguousRangeAllocatorData{
        //big page pools
        BigPageIndex* big_page_pools; //Combination of next two buffers, literally equal as a pointer to global_page_pool
        BigPageIndex* global_page_pool; //provided by arch setup as part of buffer
        BigPageIndex* local_page_pool; //provided by arch setup as part of buffer, index by ProcessorID

        //At some point we should look into profiling for false sharing or try breaking this up into
        //separate buffers with another "ownership" map that updates less frequently...
        //It's less space efficient, but almost negligibly so
        struct BigPageFreeMapping* big_page_free_map; //provided by arch setup as part of buffer

        LocalPoolInfo* local_pool_info; //kmalloc'd
        size_t  global_pool_size;

        //small page pool
        SmallPageIndex* small_page_pool; //provided by arch setup as part of buffer
        SmallPageBufferOffset* small_page_free_index; //provided by arch setup as part of buffer
        SmallPoolIndex* small_page_free_map; //provided by arch setup as part of buffer

        //bounds for the range
        phys_memory_range range;

        //max size of local pool for a single processor, cached for convenience
        size_t big_page_pool_max_size;
    private:
        inline BigPageIndex* bigPagePoolForBufferId(BufferId id){
            return (BigPageIndex*)((uint64_t)big_page_pools + id * big_page_pool_max_size);
        }

        inline BigPageIndex* bigPagePoolForProcessor(kernel::hal::ProcessorID id){
            return bigPagePoolForBufferId(fromProcID(id));
        }

        inline BigPageFreeMapping& bigPageFreeMapEntry(BigPageIndex i){
            return big_page_free_map[(size_t)i];
        }

        inline SmallPoolIndex& smallPageFreeMapEntry(BigPageIndex bi, SmallPageIndex si){
            return small_page_free_map[(size_t)bi * smallPagesPerBigPage + (size_t)si];
        }

        inline SmallPageIndex& smallPageAtIndex(BigPageIndex bi, SmallPoolIndex si){
            return small_page_pool[(size_t)bi * smallPagesPerBigPage + (size_t)si];
        }

        inline SmallPageIndex& smallPageRefFromFreeMap(BigPageIndex bi, SmallPageIndex si){
            SmallPoolIndex spi = smallPageFreeMapEntry(bi, si);
            return smallPageAtIndex(bi, spi);
        }

        inline SmallPageBufferOffset& smallPageFreeOffset(BigPageIndex bi){
            return small_page_free_index[(size_t)bi];
        }

        inline SmallPoolIndex& smallPageFreeMapBottom(BigPageIndex bi){
            return small_page_free_map[(size_t)bi * smallPagesPerBigPage + smallPageFreeOffset(bi)];
        }

        inline SmallPageIndex& smallPagePoolFreeBottom(BigPageIndex bi){
            return smallPageAtIndex(bi, (SmallPoolIndex) smallPageFreeOffset(bi));
        }

        inline BigPageIndex& bigPageAtIndex(BigPoolIndex i, BigPageIndex* pool){
            return pool[(size_t)i];
        }

        inline BigPageIndex& bigPagePoolFreeBottom(hal::ProcessorID pid){
            return bigPagePoolForProcessor(pid)[local_pool_info[pid].bottom_of_free_pool];
        }

        inline BigPageIndex& topOfLocalPool(hal::ProcessorID pid){
            return bigPagePoolForProcessor(pid)[local_pool_info[pid].local_pool_size];
        }

        inline BigPageIndex& bigPagePoolPartiallyUsedTop(hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(local_pool_info[pid].bottom_of_used_pool > 0, "Local pool has no partially used big pages");
#endif
            return bigPagePoolForProcessor(pid)[local_pool_info[pid].bottom_of_used_pool - 1];
        }

        inline BigPageIndex& bigPageRefFromFreeMap(BigPageIndex i){
            auto freeMapping = bigPageFreeMapEntry(i);
            return bigPageAtIndex(freeMapping.index, bigPagePoolForBufferId(freeMapping.bufferId));
        }

        inline bool smallPageAllocated(BigPageIndex bi, SmallPageIndex si){
            return smallPageFreeMapEntry(bi, si) < smallPageFreeMapBottom(bi);
        }

        inline bool bigPageAllocated(BigPageIndex bi){
            auto freeMapEntry = bigPageFreeMapEntry(bi);
            if(freeMapEntry.bufferId == GLOBAL_POOL){
                return false;
            }
            return (freeMapEntry.bufferId < local_pool_info[freeMapEntry.bufferId - 1].bottom_of_free_pool)
            && (freeMapEntry.bufferId >= local_pool_info[freeMapEntry.bufferId - 1].bottom_of_used_pool);
        }

        // Helper function to swap big pages and their corresponding free map entries
        __attribute__((always_inline)) void swapBigPages(BigPageIndex& a, BigPageIndex& b) {
            swap(bigPageFreeMapEntry(a), bigPageFreeMapEntry(b));
            swap(a, b);
        }

        // Moves a free big page to the allocated set
        __attribute__((always_inline)) void moveFreeBigPageToAllocated(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfFreePool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfFreePool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_free_pool, "bottomOfFreePool not correctly set!");
            assert(bigPagePoolForBufferId(bigPageFreeMapEntry(i).bufferId) == localPool, "localPool set incorrectly!");
            assert((size_t) bigPageFreeMapEntry(i).index >= bottomOfFreePool, "big page should be free");
#endif
            swapBigPages(i, localPool[bottomOfFreePool]);
            bottomOfFreePool++;
        }

        // Moves an allocated big page back to the free pool
        __attribute__((always_inline)) void moveAllocatedBigPageToFree(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfFreePool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfFreePool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_free_pool, "bottomOfFreePool not correctly set!");
            assert(bigPagePoolForBufferId(bigPageFreeMapEntry(i).bufferId) == localPool, "localPool set incorrectly!");
            assert((size_t) bigPageFreeMapEntry(i).index < bottomOfFreePool, "big page should be allocated");
#endif
            swapBigPages(i, localPool[bottomOfFreePool - 1]);
            bottomOfFreePool--;
        }

        // Moves an allocated big page to the partially allocated set
        __attribute__((always_inline)) void moveAllocatedBigPageToPartiallyAllocated(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfUsedPool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfUsedPool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_used_pool, "bottomOfUsedPool not correctly set!");
            assert(bigPagePoolForBufferId(bigPageFreeMapEntry(i).bufferId) == localPool, "localPool set incorrectly!");
            assert((size_t) bigPageFreeMapEntry(i).index >= bottomOfUsedPool, "big page should be allocated");
#endif
            swapBigPages(i, localPool[bottomOfUsedPool]);
            bottomOfUsedPool++;
        }

        // Moves a partially allocated big page back to the fully allocated set
        __attribute__((always_inline)) void movePartiallyAllocatedBigPageToAllocated(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfUsedPool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfUsedPool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_used_pool, "bottomOfUsedPool not correctly set!");
            assert(bigPagePoolForBufferId(bigPageFreeMapEntry(i).bufferId) == localPool, "localPool set incorrectly!");
            assert((size_t) bigPageFreeMapEntry(i).index < bottomOfUsedPool, "big page should be partially allocated");
#endif
            swapBigPages(i, localPool[bottomOfUsedPool - 1]);
            bottomOfUsedPool--;
        }

        // Moves a free big page to the partially allocated set
        __attribute__((always_inline)) void moveFreeBigPageToPartiallyAllocated(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfFreePool, BigPageBufferOffset& bottomOfUsedPool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfUsedPool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_used_pool, "bottomOfUsedPool not correctly set!");
            assert(&bottomOfFreePool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_free_pool, "bottomOfFreePool not correctly set!");
            assert(bigPagePoolForBufferId(bigPageFreeMapEntry(i).bufferId) == localPool, "localPool set incorrectly!");
            assert((size_t) bigPageFreeMapEntry(i).index >= bottomOfFreePool, "big page should be free");
#endif
            BigPageIndex& bottomUsed = localPool[bottomOfUsedPool];
            BigPageIndex& bottomFree = localPool[bottomOfFreePool];
            rotateRight(bigPageFreeMapEntry(bottomUsed), bigPageFreeMapEntry(bottomFree), bigPageFreeMapEntry(i));
            rotateRight(bottomUsed, bottomFree, i);
            bottomOfUsedPool++;
            bottomOfFreePool++;
        }

        // Moves a partially allocated big page back to the free pool
        __attribute__((always_inline)) void movePartiallyAllocatedBigPageToFree(BigPageIndex& i, BigPageIndex* localPool, BigPageBufferOffset& bottomOfFreePool, BigPageBufferOffset& bottomOfUsedPool) {
#ifdef ALLOCATOR_DEBUG
            assert(&bottomOfUsedPool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_used_pool, "bottomOfUsedPool not correctly set!");
            assert(&bottomOfFreePool == &local_pool_info[bigPageFreeMapEntry(i).bufferId - 1].bottom_of_free_pool, "bottomOfFreePool not correctly set!");
            assert((size_t) bigPageFreeMapEntry(i).index < bottomOfUsedPool, "big page should be partially allocated");
#endif
            BigPageIndex& topOfUsed = localPool[bottomOfFreePool - 1];
            BigPageIndex& topOfPartiallyAllocated = localPool[bottomOfUsedPool - 1];
            rotateLeft(bigPageFreeMapEntry(topOfUsed), bigPageFreeMapEntry(topOfPartiallyAllocated), bigPageFreeMapEntry(i));
            rotateLeft(topOfUsed, topOfPartiallyAllocated, i);
            bottomOfUsedPool--;
            bottomOfFreePool--;
        }

        void claimBigPageFromGlobalPool(hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(global_pool_size > 0, "Tried to take big page from empty global pool");
#endif
            BigPageIndex& topOfGlobalPool = global_page_pool[global_pool_size - 1];
            //Shrink the global pool
            global_pool_size--;
            //Set the top of the local pool to i1
            bigPagePoolForProcessor(pid)[local_pool_info[pid].local_pool_size] = topOfGlobalPool;
            //update the free mapping
            bigPageFreeMapEntry(topOfGlobalPool).bufferId = fromProcID(pid);
            bigPageFreeMapEntry(topOfGlobalPool).index = (BigPoolIndex)local_pool_info[pid].local_pool_size;
            //grow the local pool to see the new free big page
            local_pool_info[pid].local_pool_size++;
        }

        void claimBigPagesFromGlobalPool(size_t count, hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(global_pool_size >= count, "Tried to take big page from empty global pool");
#endif
            //Copy the big page indices to the top of the local pool
            memcpy(&topOfLocalPool(pid), &global_page_pool[global_pool_size - count], sizeof(BigPageIndex) * count);
            //Shrink the global pool
            global_pool_size -= count;
            //update the free mappings
            auto lpool = bigPagePoolForProcessor(pid);
            for(size_t i = 0; i < count; i++){
                auto bi = (BigPoolIndex)(local_pool_info[pid].local_pool_size + i);
                bigPageFreeMapEntry(lpool[(size_t)bi]).index = bi;
                bigPageFreeMapEntry(lpool[(size_t)bi]).bufferId = fromProcID(pid);
            }
            //grow the local pool to see the new free big page
            local_pool_info[pid].local_pool_size += count;
        }

        void claimSpecificBigPageFromGlobalPool(BigPageIndex bigPageIdx, hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bigPageIdx).bufferId == GLOBAL_POOL, "Tried to take big page from a local pool");
#endif
            //Move i1 to the top of the global pool
            BigPageIndex& topOfGlobalPool = global_page_pool[global_pool_size - 1];
            BigPageIndex& targetBigPage = bigPageRefFromFreeMap(bigPageIdx);
            swapBigPages(topOfGlobalPool, targetBigPage);
            claimBigPageFromGlobalPool(pid);
        }

        //This should be called rarely... so I'm far more concerned about correctness than with speed
        void reserveSmallPage(BigPageIndex bi, SmallPageIndex si, hal::ProcessorID pid) {
            //If the small page has already been allocated, we probably reserved it already. This could happen, for example,
            //after the memory allocator has been initialized, and the kernel tries to reserve its own pages.
            //At this point, it seems harmless (especially since we should no longer be reserving pages after a certain
            //point early in the boot process, but allocating them)
            if(smallPageAllocated(bi, si)){
                assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "Prereserved small page sanity check failed");
                return;
            }
            // If the big page belongs to the global pool, claim it for the current processor.
            if (bigPageFreeMapEntry(bi).bufferId == GLOBAL_POOL) {
                claimSpecificBigPageFromGlobalPool(bi, pid);
            }

#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid),
                   "Tried to reserve small page from wrong pool");
#endif

            // If the big page is free in the local pool, mark it as partially allocated.
            if ((size_t)bigPageFreeMapEntry(bi).index >= local_pool_info[pid].bottom_of_free_pool) {
                moveFreeBigPageToPartiallyAllocated(
                        bigPageRefFromFreeMap(bi),
                        bigPagePoolForProcessor(pid),
                        local_pool_info[pid].bottom_of_free_pool,
                        local_pool_info[pid].bottom_of_used_pool
                        );
                local_pool_info[pid].free_small_pages_in_partial_allocs += smallPagesPerBigPage;
            }

#ifdef ALLOCATOR_DEBUG
            assert((size_t)bigPageFreeMapEntry(bi).index < local_pool_info[pid].bottom_of_used_pool,
                   "Tried to reserve small page in fully allocated big page");
#endif

            // Get references to the target small page and its metadata.
            SmallPageIndex& targetPage = smallPageRefFromFreeMap(bi, si);
            SmallPoolIndex& targetPoolIndex = smallPageFreeMapEntry(bi, si);

            // Get references to the last free small page in this big page.
            SmallPageIndex& lastFreePage = smallPagePoolFreeBottom(bi);
            SmallPoolIndex& lastFreePoolIndex = smallPageFreeMapBottom(bi);

#ifdef ALLOCATOR_DEBUG
            assert((size_t)targetPoolIndex >= (size_t)smallPageFreeOffset(bi),
                   "Tried to reserve an already reserved small page");
#endif

            // Swap the target small page with the last free small page.
            swap(targetPage, lastFreePage);
            swap(targetPoolIndex, lastFreePoolIndex);

            // Mark this small page as allocated.
            smallPageFreeOffset(bi)++;
            local_pool_info[pid].free_small_pages_in_partial_allocs--;

            // If all small pages in this big page have been allocated, move it to the fully allocated zone.
            if (smallPageFreeOffset(bi) == SMALL_POOL_FULL) {
                movePartiallyAllocatedBigPageToAllocated(
                        bigPageRefFromFreeMap(bi),
                        bigPagePoolForProcessor(pid),
                        local_pool_info[pid].bottom_of_used_pool
                        );
            }
        }

        void reserveBigPage(BigPageIndex bi, hal::ProcessorID pid){
            //Just as with the small page case, it's conceivable that we might try to reserve a big page twice for
            //legitimate reasons. If we see the page has already been reserved, run a sanity check
            if(bigPageAllocated(bi)){
                assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "Rereservation sanity check failed");
                return;
            }
            //First check if the big page belongs to the global pool, and if so, claim ownership of it
            if(bigPageFreeMapEntry(bi).bufferId == GLOBAL_POOL){
                claimSpecificBigPageFromGlobalPool(bi, pid);
            }
#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "Tried to reserve small page from wrong pool");
            assert((size_t) bigPageFreeMapEntry(bi).index >= local_pool_info[pid].bottom_of_free_pool,
                   "Tried to reserve big page that's already in use");
#endif
            //It's also possible that we already reserved small sub-pages of this big page, in which case
            //we need to handle this specially.
            if((size_t)bigPageFreeMapEntry(bi).index < (size_t)local_pool_info[pid].bottom_of_used_pool){
                movePartiallyAllocatedBigPageToAllocated(bigPageRefFromFreeMap(bi), bigPagePoolForProcessor(pid),
                                           local_pool_info[pid].bottom_of_used_pool);
            }
            else {
                moveFreeBigPageToAllocated(bigPageRefFromFreeMap(bi), bigPagePoolForProcessor(pid),
                                           local_pool_info[pid].bottom_of_free_pool);
            }
        }

        SmallPageIndex allocateSmallPage(BigPageIndex i, hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(smallPageFreeOffset(i) < SMALL_POOL_FULL, "tried to allocate small page from full big page");
#endif
            SmallPageIndex out = smallPagePoolFreeBottom(i);
            smallPageFreeOffset(i)++;
            if(smallPageFreeOffset(i) == SMALL_POOL_FULL){
                movePartiallyAllocatedBigPageToAllocated(i, bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_used_pool);
            }
            local_pool_info[pid].free_small_pages_in_partial_allocs--;
            return out;
        }

        void allocateSmallPages(Vector<phys_addr> small_pages, size_t max_pagecount, BigPageIndex bi, hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
            assert(smallPageFreeOffset(bi) < SMALL_POOL_FULL, "tried to allocate small page from full big page");
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "tried to allocate from big page belonging to wrong pool");
#endif
            auto freeOffset = smallPageFreeOffset(bi);
            size_t allocatable = min(max_pagecount, (size_t)(SMALL_POOL_FULL - freeOffset));
            for(size_t n = 0; n < allocatable; n++){
                small_pages.push(getSmallPageAddress(bi, smallPageAtIndex(bi, (SmallPoolIndex)(freeOffset + n)), range));
            }
            smallPageFreeOffset(bi) += (SmallPageBufferOffset)allocatable;
            if(smallPageFreeOffset(bi) == SMALL_POOL_FULL){
                movePartiallyAllocatedBigPageToAllocated(bi, bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_used_pool);
            }
        }

        void freeSmallPageInternal(BigPageIndex bi, SmallPageIndex si){
            SmallPoolIndex& toSwapFreeMap = smallPageFreeMapEntry(bi, si);
            SmallPoolIndex& usedTopFreeMap = smallPageFreeMapEntry(bi,
                                           (SmallPageIndex)(small_page_free_index[(size_t)bi] - 1));
            SmallPageIndex& toSwap = smallPageAtIndex(bi, toSwapFreeMap);
            SmallPageIndex& usedTop = smallPageAtIndex(bi, usedTopFreeMap);
            swap(toSwapFreeMap, usedTopFreeMap);
            swap(toSwap, usedTop);
            small_page_free_index[(size_t)bi]--;
        }

    public:

        void reservePhysMemoryRange(phys_memory_range to_reserve, hal::ProcessorID pid){
            //if the ranges are disjoint, return early
            //also be sure to reservation of pages just outside range, for initialization purposes

            uint64_t rangeTop = roundUpToNearestMultiple(range.end.value, bigPageSize);
            uint64_t rangeBot = roundDownToNearestMultiple(range.start.value, bigPageSize);
            if(to_reserve.start.value >= rangeTop){
                return;
            }
            if(to_reserve.end.value <= rangeBot){
                return;
            }

            uint64_t bottom = max(to_reserve.start.value, rangeBot);
            uint64_t top = min(to_reserve.end.value, rangeTop);
            //if we're trying to reserve a memory range of 0 size, just bail
            if(bottom == top){
                return;
            }
            //page align our endpoints
            bottom = roundDownToNearestMultiple(bottom, smallPageSize);
            top = roundUpToNearestMultiple(top, smallPageSize);

            phys_addr toReserve(bottom);

            while(toReserve.value < top){
                //if we can reserve a big page, do it
                if((toReserve.value % bigPageSize == 0) && (toReserve.value + bigPageSize <= top)){
                    reserveBigPage(getBigPageIndexFromAddress(toReserve, range), pid);
                    toReserve.value += bigPageSize;
                }
                else{
                    reserveSmallPage(getBigPageIndexFromAddress(toReserve, range),
                                     getSmallPageIndexFromAddress(toReserve), pid);
                    toReserve.value += smallPageSize;
                }
            }
        }

        void reserveOverlap(hal::ProcessorID pid){
            phys_addr bigPageAlignedTop = phys_addr(roundUpToNearestMultiple(range.end.value, bigPageSize));
            phys_addr bigPageAlignedBot = phys_addr(roundDownToNearestMultiple(range.start.value, bigPageSize));

            reservePhysMemoryRange({bigPageAlignedBot, range.start}, pid); //Reserve stuff below the start of the memory range
            reservePhysMemoryRange({range.end, bigPageAlignedTop}, pid); //Reserve stuff above the end of the memory range
        }

        ContiguousRangeAllocatorData(page_allocator_range_info info, size_t processor_count) : range(info.range){
            uint64_t regionTopBigPageAddr = roundUpToNearestMultiple(range.end.value, bigPageSize);
            uint64_t regionBottomBigPageAddr = roundDownToNearestMultiple(range.start.value, bigPageSize);

            size_t regionSizeInBytes = regionTopBigPageAddr - regionBottomBigPageAddr;
            size_t bigPageCount = divideAndRoundUp(regionSizeInBytes, bigPageSize);
            size_t smallPageCount = bigPageCount * smallPagesPerBigPage;
            (void)smallPageCount;

            big_page_pool_max_size = roundUpToNearestMultiple(bigPageCount * sizeof(BigPageIndex),
                                                           kernel::hal::CACHE_LINE_SIZE);
            //Divide up our buffer given to us
            uint64_t buff_vaddr = (uint64_t)info.buffer_start;
            big_page_pools = (BigPageIndex *)buff_vaddr;
            global_page_pool = (BigPageIndex *)buff_vaddr;
            buff_vaddr += big_page_pool_max_size;
            local_page_pool = (BigPageIndex *)buff_vaddr;
            buff_vaddr += big_page_pool_max_size * processor_count;
            big_page_free_map = (struct BigPageFreeMapping*)buff_vaddr;
            buff_vaddr += roundUpToNearestMultiple(bigPageCount * sizeof(BigPageFreeMapping),
                                                   kernel::hal::CACHE_LINE_SIZE);
            small_page_pool = (SmallPageIndex*)buff_vaddr;
            buff_vaddr += roundUpToNearestMultiple(smallPageCount * sizeof(SmallPageIndex),
                                                   kernel::hal::CACHE_LINE_SIZE);
            small_page_free_map = (SmallPoolIndex*)buff_vaddr;
            buff_vaddr += roundUpToNearestMultiple(smallPageCount * sizeof(SmallPoolIndex),
                                                   kernel::hal::CACHE_LINE_SIZE);
            small_page_free_index = (SmallPageBufferOffset*)buff_vaddr;
            buff_vaddr += roundUpToNearestMultiple(bigPageCount * sizeof(SmallPageBufferOffset),
                                                   kernel::hal::CACHE_LINE_SIZE);
            //Purely a sanity check
            assert(buff_vaddr - (uint64_t)info.buffer_start <= requestedBufferSizeForRange(range, processor_count),
                   "Somehow we're using more of the buffer than we asked for???");

            //Allocate an array for the local pool info in the "heap" (bump allocator for now), init
            local_pool_info = new LocalPoolInfo[processor_count];
            for(size_t i = 0; i < processor_count; i++){
                local_pool_info[i].bottom_of_free_pool = BigPageBufferOffset{0};
                local_pool_info[i].bottom_of_used_pool = BigPageBufferOffset{0};
                local_pool_info[i].free_small_pages_in_partial_allocs = 0;
                local_pool_info[i].local_pool_size = 0;
            }

            global_pool_size = bigPageCount;
            //small_page_free_index doesn't need to be cleared because we require that
            //the buffer is zeroed out. Similarly, the local page pools don't need to be populated

            for(size_t i = 0; i < bigPageCount; i++){
                BigPageIndex bi {(BigIndexRawType) i};
                BigPoolIndex bpi{(SmallIndexRawType) i};
                //Give every big page to the global pool
                global_page_pool[i] = BigPageIndex{bi};
                big_page_free_map[i] = {bpi, GLOBAL_POOL};
                //for every big page, init the corresponding part of the small page pool
                for(size_t j = 0; j < smallPagesPerBigPage; j++){
                    SmallPageIndex si{(SmallIndexRawType) j};
                    SmallPoolIndex spi{(SmallIndexRawType) j};
                    small_page_pool[getIndexIntoSmallPagePool(bpi, spi)] = si;
                    small_page_free_map[getIndexIntoSmallPagePool(bpi, spi)] = spi;
                }
            }
            //At this point, we just need to reserve any small pages not contained in the memory range
            reserveOverlap(0);

        }

        [[nodiscard]]
        size_t getFreeLocalBigPages(hal::ProcessorID pid) const{
            return local_pool_info[pid].local_pool_size - local_pool_info[pid].bottom_of_free_pool;
        }

        [[nodiscard]]
        //returns the total number of free small pages available in the local pool (including those possibly storable
        //in unallocated big pages)
        size_t getFreeLocalSmallPages(hal::ProcessorID pid) const{
            return local_pool_info[pid].free_small_pages_in_partial_allocs + getFreeLocalBigPages(pid) * smallPagesPerBigPage;
        }

        [[nodiscard]]
        phys_addr allocateSmallPage(hal::ProcessorID pid){
            //If we don't have any partially allocated pages, move a big page down to the partially occupied zone
            if(local_pool_info[pid].bottom_of_used_pool == 0){
#ifdef ALLOCATOR_DEBUG
                assert(getFreeLocalBigPages(pid) > 0, "Tried to allocate a small page from an empty local pool!");
#endif
                moveFreeBigPageToPartiallyAllocated(bigPagePoolFreeBottom(pid), bigPagePoolForProcessor(pid),
                                                    local_pool_info[pid].bottom_of_free_pool, local_pool_info[pid].bottom_of_used_pool);
            }
            BigPageIndex& bi = bigPagePoolPartiallyUsedTop(pid);
            SmallPageIndex si = allocateSmallPage(bi, pid);
            return getSmallPageAddress(bi, si, range);
        }

        [[nodiscard]]
        phys_addr allocateBigPage(hal::ProcessorID pid){
#ifdef ALLOCATOR_DEBUG
                assert(getFreeLocalBigPages(pid) > 0, "Tried to allocate a big page from an empty local pool!");
#endif
            BigPageIndex& bi = bigPagePoolFreeBottom(pid);
            local_pool_info[pid].bottom_of_free_pool++;
            return getBigPageAddress(bi, range);
        }

        size_t stealBigPagesFromGlobalPool(size_t requestedBigPages, hal::ProcessorID pid){
            size_t allocatable = min(requestedBigPages, global_pool_size);
            claimBigPagesFromGlobalPool(allocatable, pid);
            return allocatable;
        }

        bool addressInRange(phys_addr addr){
            return (addr.value >= range.start.value) && (addr.value < range.end.value);
        }

        bool isAddressInLocalPoolForProcessor(phys_addr addr, hal::ProcessorID  pid){
            auto bi = getBigPageIndexFromAddress(addr, range);
            return bigPageFreeMapEntry(bi).bufferId == fromProcID(pid);
        }

        void freeSmallPage(phys_addr addr, hal::ProcessorID pid){
            BigPageIndex bi = getBigPageIndexFromAddress(addr, range);
            SmallPageIndex si = getSmallPageIndexFromAddress(addr);
#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "PID mismatch");
            assert((size_t)smallPageFreeOffset(bi) > (size_t)smallPageFreeMapEntry(bi, si), "Tried to free unallocated small page");
#endif
            freeSmallPageInternal(bi, si);
            local_pool_info[pid].free_small_pages_in_partial_allocs++;
            //If we freed the last small page, move the big page to the freed zone
            if(small_page_free_index[(size_t)bi] == 0){
                movePartiallyAllocatedBigPageToFree(bi, bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_free_pool, local_pool_info[pid].bottom_of_used_pool);
                local_pool_info[pid].free_small_pages_in_partial_allocs -= smallPagesPerBigPage;
            }
            //If the big page used to be full, move it to the partially allocated zone
            if(small_page_free_index[(size_t)bi] == SMALL_POOL_FULL - 1){
                moveAllocatedBigPageToPartiallyAllocated(bi, bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_used_pool);
            }
        }

        void freeBigPage(phys_addr addr, hal::ProcessorID pid){
            BigPageIndex bi = getBigPageIndexFromAddress(addr, range);
#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "PID mismatch");
            assert((size_t)bigPageFreeMapEntry(bi).index < local_pool_info[pid].bottom_of_free_pool,
                   "Tried to free already freed big page");
            assert((size_t)bigPageFreeMapEntry(bi).index >= local_pool_info[pid].bottom_of_used_pool,
                   "Tried to free big page that is only partially allocated");
#endif
            moveAllocatedBigPageToFree(bi, bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_free_pool);
        }
    };

    size_t requestedBufferSizeForRange(mm::phys_memory_range range, size_t processor_count){
        uint64_t regionTopBigPageAddr = roundUpToNearestMultiple(range.end.value, bigPageSize);
        uint64_t regionBottomBigPageAddr = roundDownToNearestMultiple(range.start.value, bigPageSize);

        size_t regionSizeInBytes = regionTopBigPageAddr - regionBottomBigPageAddr;
        size_t bigPageCount = divideAndRoundUp(regionSizeInBytes, bigPageSize);
        size_t smallPageCount = bigPageCount * smallPagesPerBigPage;
        size_t out = 0;
        //global_page_pool
        out += roundUpToNearestMultiple(bigPageCount * sizeof(BigPageIndex), kernel::hal::CACHE_LINE_SIZE);
        //local_page_pool
        out += roundUpToNearestMultiple(bigPageCount * sizeof(BigPageIndex),
                                        kernel::hal::CACHE_LINE_SIZE) * processor_count;
        //big_page_free_map
        out += roundUpToNearestMultiple(bigPageCount * sizeof(BigPageFreeMapping), kernel::hal::CACHE_LINE_SIZE);
        //small_page_free_index
        out += roundUpToNearestMultiple(bigPageCount * sizeof(SmallPageBufferOffset), kernel::hal::CACHE_LINE_SIZE);
        //small_page_pool
        out += roundUpToNearestMultiple(smallPageCount * sizeof(SmallPageIndex), kernel::hal::CACHE_LINE_SIZE);
        //small_page_free_map
        out += roundUpToNearestMultiple(smallPageCount * sizeof(SmallPoolIndex), kernel::hal::CACHE_LINE_SIZE);
        return out;
    }

    Vector<ContiguousRangeAllocatorData>* allocators;

    void init(Vector<page_allocator_range_info>& infos, size_t processor_count){
        allocators = new Vector<ContiguousRangeAllocatorData>(infos.getSize());
        kernel::DbgOut << "initializing page allocators\n";
        //Initialize our locks
        local_locks = new hal::spinlock_t[processor_count];

        for(size_t x = 0; x < processor_count; x++){
            local_locks[x] = hal::SPINLOCK_INITIALIZER;
        }

        global_lock = hal::SPINLOCK_INITIALIZER;

        for(auto info : infos){
            allocators->push(ContiguousRangeAllocatorData(info, processor_count));
        }
    }

    //TODO maybe make this tunable at some point?
    const size_t STEAL_EXTRA_REQUESTED_PAGES = 4;

    bool tryStealPages(size_t requiredBigPages, hal::ProcessorID pid){
        size_t requestedPages = requiredBigPages + STEAL_EXTRA_REQUESTED_PAGES; //
        hal::acquire_spinlock(global_lock);
        for(auto& allocator : *allocators){
            if(allocator.global_pool_size > 0){
                requestedPages -= allocator.stealBigPagesFromGlobalPool(requestedPages, pid);
            }
            if(requestedPages <= STEAL_EXTRA_REQUESTED_PAGES){
                hal::release_spinlock(global_lock);
                return true;
            }
        }
        hal::release_spinlock(global_lock);
        //TODO support stealing from other processors
        return false;
    }

    void tryDonatePagesIfNecessary(hal::ProcessorID pid){
        (void)pid;
        //TODO
    }

    void reservePhysicalRange(phys_memory_range range){
        for(auto& allocator : *allocators){
            allocator.reservePhysMemoryRange(range, 0);
        }
    }

    // Allocates a small page and returns its physical address
    phys_addr allocateSmallPage(){
        auto pid = hal::getCurrentProcessorID();
        phys_addr out(nullptr);
        hal::acquire_spinlock(local_locks[pid]);
        //Try to allocate, steal if necessary, then try to allocate again
        while(true){
            for(auto& allocator : *allocators){
                if(allocator.getFreeLocalSmallPages(pid) > 0){
                    out = allocator.allocateSmallPage(pid);
                    hal::release_spinlock(local_locks[pid]);
                    return out;
                }
            }
            if(!tryStealPages(1, pid)){
                hal::release_spinlock(local_locks[pid]);
                return out;
            }
        }
    }

    // Allocates a big page and returns its physical address
    phys_addr allocateBigPage(){
        auto pid = hal::getCurrentProcessorID();
        phys_addr out(nullptr);
        hal::acquire_spinlock(local_locks[pid]);
        //Try to allocate, steal if necessary, then try to allocate again
        while(true){
            for(auto& allocator : *allocators){
                if(allocator.getFreeLocalBigPages(pid) > 0){
                    out = allocator.allocateBigPage(pid);
                    hal::release_spinlock(local_locks[pid]);
                    return out;
                }
            }
            if(!tryStealPages(1, pid)){
                hal::release_spinlock(local_locks[pid]);
                return out;
            }
        }
    }

    //Frees a small page that was allocated to the local processor's pool
    //This notion of a "local" free isn't exactly super meaningful for the case of freeing a single page
    //It only skips one comparison. But the analogous notion for bulk frees should be more meaningful.
    void freeLocalSmallPage(phys_addr page){
        auto pid = hal::getCurrentProcessorID();
        for(auto& allocator : *allocators){
            if(allocator.addressInRange(page)){
                hal::acquire_spinlock(local_locks[pid]);
                allocator.freeSmallPage(page, pid);
                tryDonatePagesIfNecessary(pid);
                hal::release_spinlock(local_locks[pid]);
                return;
            }
        }
        assertNotReached("Tried to free page outside of range of allocators");
    }

    //Frees a big page that was allocated to the local processor's pool
    //This notion of a "local" free isn't exactly super meaningful for the case of freeing a single page
    //It only skips one comparison. But the analogous notion for bulk frees should be more meaningful.
    void freeLocalBigPage(phys_addr page){
        auto pid = hal::getCurrentProcessorID();
        for(auto& allocator : *allocators){
            if(allocator.addressInRange(page)){
                hal::acquire_spinlock(local_locks[pid]);
                allocator.freeBigPage(page, pid);
                tryDonatePagesIfNecessary(pid);
                hal::release_spinlock(local_locks[pid]);
                return;
            }
        }
        assertNotReached("Tried to free page outside of range of allocators");
    }

    // Frees a small page (with no restriction on the which pool the page belongs to)
    void freeSmallPage(phys_addr page){
        for(auto& allocator : *allocators){
            if(allocator.addressInRange(page)){
                auto bid = allocator.big_page_free_map[(size_t)getBigPageIndexFromAddress(page, allocator.range)].bufferId;
#ifdef ALLOCATOR_DEBUG
                assert(bid != 0, "Tried to free unowned page.");
#endif
                auto pid = (hal::ProcessorID)(bid - 1);
                hal::acquire_spinlock(local_locks[pid]);
                allocator.freeSmallPage(page, pid);
                tryDonatePagesIfNecessary(pid);
                hal::release_spinlock(local_locks[pid]);
            }
        }
        assertNotReached("Tried to free page outside of range of allocators");
    }

    // Frees a big page (with no restriction on the which pool the page belongs to)
    void freeBigPage(phys_addr page){
        for(auto& allocator : *allocators){
            if(allocator.addressInRange(page)){
                auto bid = allocator.big_page_free_map[(size_t)getBigPageIndexFromAddress(page, allocator.range)].bufferId;
#ifdef ALLOCATOR_DEBUG
                assert(bid != 0, "Tried to free unowned page.");
#endif
                auto pid = (hal::ProcessorID)(bid - 1);
                hal::acquire_spinlock(local_locks[pid]);
                allocator.freeBigPage(page, pid);
                tryDonatePagesIfNecessary(pid);
                hal::release_spinlock(local_locks[pid]);
            }
        }
        assertNotReached("Tried to free page outside of range of allocators");
    }

    inline BulkAllocationPolicy getLocalAllocationPolicy(hal::ProcessorID pid, size_t availableSmallPagesInLocalPools, size_t availableBigPagesInLocalPools, size_t smallPagesInPartiallyUsedBigPages){
        (void)pid;
        (void)availableSmallPagesInLocalPools;
        (void)availableBigPagesInLocalPools;
        if(smallPagesInPartiallyUsedBigPages > smallPagesPerBigPage * 4 * allocators -> getSize()){
            return BulkAllocationPolicy::USE_UP_SMALL;
        }
        else{
            return BulkAllocationPolicy::PREFER_BIG;
        }
    }

    // Allocates enough pages to satisfy a request for a certain capacity
    // Returns vectors of small and big pages to fulfill the request
    bool allocatePages(size_t requestedCapacityInBytes, Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages){
        //First acquire a lock for our local pool
        auto pid = hal::getCurrentProcessorID();
        hal::acquire_spinlock(local_locks[pid]);
        //Determine how much space is available in the appropriate local pool of each allocator
        size_t availableSmallPagesInLocalPools = 0;
        size_t availableBigPagesInLocalPools = 0;
        for(auto& allocator : *allocators){
            availableSmallPagesInLocalPools += allocator.getFreeLocalSmallPages(pid);
            availableBigPagesInLocalPools += allocator.getFreeLocalBigPages(pid);
        }
        size_t smallPagesInPartiallyUsedBigPages = availableSmallPagesInLocalPools - availableBigPagesInLocalPools * smallPagesPerBigPage;
        //If we don't have enough capacity, try to steal enough pages to fulfill the request
        if(requestedCapacityInBytes > availableSmallPagesInLocalPools * smallPageSize){
            size_t neededBigPages = divideAndRoundUp(requestedCapacityInBytes - availableSmallPagesInLocalPools * smallPageSize,
                                                     bigPageSize);
            if(!tryStealPages(neededBigPages, pid)){
                //if we aren't able to steal the pages, bail out!
                hal::release_spinlock(local_locks[pid]);
                return false;
            }
        }
        auto allocationPolicy = getLocalAllocationPolicy(pid, availableSmallPagesInLocalPools, availableBigPagesInLocalPools, smallPagesInPartiallyUsedBigPages);
        if(allocationPolicy == BulkAllocationPolicy::USE_UP_SMALL){
            //Our aim is to first use up whatever partially allocated big pages we have at our disposal
            size_t requestedSmallPages = divideAndRoundUp(requestedCapacityInBytes, smallPageSize);
            //Minimize reallocations in vector by preallocating some space in our vector of small pages
            smallPages.ensureRoom(min(requestedSmallPages, smallPagesInPartiallyUsedBigPages));
            for(auto& allocator : *allocators){
                //As long as the allocator has partially allocated large pages, keep allocating small pages
                while(allocator.local_pool_info[pid].bottom_of_used_pool > 0){
                    smallPages.push(allocator.allocateSmallPage(pid));
                    requestedSmallPages--;
                    if(requestedSmallPages == 0){
                        hal::release_spinlock(local_locks[pid]);
                        return true;
                    }
                }
            }
            //If we made it here, none of the allocators should have any remaining partially allocated big pages
            //in the corresponding local pool. Thus, we just need to allocate enough small pages so that our remaining
            //requestedSmallPages is a multiple of smallPagesPerBigPage
            for(auto& allocator : *allocators){
                if(allocator.getFreeLocalBigPages(pid) > 0){
                    while(requestedSmallPages % smallPagesPerBigPage != 0){
                        smallPages.push(allocator.allocateSmallPage(pid));
                        requestedSmallPages--;
                    }
                    break;
                }
            }
            //Now we can fulfill the rest of our allocation with big pages
            bigPages.ensureRoom(requestedSmallPages / smallPagesPerBigPage);
            for(auto& allocator : *allocators){
                while(allocator.getFreeLocalBigPages(pid) > 0){
                    bigPages.push(allocator.allocateBigPage(pid));
                    requestedSmallPages -= smallPagesPerBigPage;
                    if(requestedSmallPages == 0){
                        hal::release_spinlock(local_locks[pid]);
                        return true;
                    }
                }
            }
        }
        else if(allocationPolicy == BulkAllocationPolicy::PREFER_BIG){
            size_t requestedSmallPages = divideAndRoundUp(requestedCapacityInBytes, smallPageSize);
            //First allocate as much memory as we can with big pages
            for(auto& allocator : *allocators){
                while(allocator.getFreeLocalBigPages(pid) > 0 && requestedSmallPages >= smallPagesPerBigPage){
                    bigPages.push(allocator.allocateBigPage(pid));
                    requestedSmallPages -= smallPagesPerBigPage;
                }
                if(requestedSmallPages > smallPagesPerBigPage){
                    break;
                }
            }
            //Preallocate enough room in the small pages vector
            smallPages.ensureRoom(requestedSmallPages);
            //Then take small pages from whatever allocators have partially used big pages
            for(auto& allocator : *allocators){
                while(allocator.local_pool_info[pid].bottom_of_used_pool > 0){
                    smallPages.push(allocator.allocateSmallPage(pid));
                    requestedSmallPages--;
                    if(requestedSmallPages == 0){
                        hal::release_spinlock(local_locks[pid]);
                        return true;
                    }
                }
            }
            //Finally, fill in the rest of the small pages from the first allocator that has space
            for(auto& allocator : *allocators){
                if(allocator.getFreeLocalBigPages(pid) > 0){
                    while(requestedSmallPages > 0){
                        smallPages.push(allocator.allocateSmallPage(pid));
                        requestedSmallPages--;
                    }
                }
            }
        }
        hal::release_spinlock(local_locks[pid]);
        return true;
    }

    // Frees a set of pages that were allocated to the local processor's pool
    void freeLocalPages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages){
        //TODO change this method to first sort the vectors so we don't need to do any nested iteration
        //This is an initial "good enough" implementation so I can move on to more fun stuff
        auto pid = hal::getCurrentProcessorID();
        hal::acquire_spinlock(local_locks[pid]);
        for(auto& allocator : *allocators){
            for(size_t i = 0; i < smallPages.getSize(); i++){
                if(allocator.addressInRange(smallPages[i])){
                    allocator.freeSmallPage(smallPages[i], pid);
                    smallPages.remove(i--);
                }
            }
            for(size_t i = 0; i < bigPages.getSize(); i++){
                if(allocator.addressInRange(bigPages[i])){
                    allocator.freeBigPage(bigPages[i], pid);
                    bigPages.remove(i--);
                }
            }
        }
        tryDonatePagesIfNecessary(pid);
        hal::release_spinlock(local_locks[pid]);
        assert(smallPages.getSize() == 0 && bigPages.getSize() == 0, "Tried to free pages that were out of allocator ranges");
    }

    // Frees a set of pages (with no restriction on the which pools the pages belong to)
    void freePages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages){
        //FIXME this is really a horrendously bad way of doing things
        for(uint8_t pid = 0; pid < hal::processorCount(); pid++) {
            //TODO change this method to first sort the vectors so we don't need to do any nested iteration
            //This is an initial "good enough" implementation so I can move on to more fun stuff
            hal::acquire_spinlock(local_locks[pid]);
            for (auto &allocator: *allocators) {
                for (size_t i = 0; i < smallPages.getSize(); i++) {
                    if (allocator.addressInRange(smallPages[i])) {
                        if(allocator.isAddressInLocalPoolForProcessor(smallPages[i], pid)){
                            allocator.freeSmallPage(smallPages[i], pid);
                            smallPages.remove(i--);
                        }
                    }
                }
                for (size_t i = 0; i < bigPages.getSize(); i++) {
                    if (allocator.addressInRange(bigPages[i])) {
                        if(allocator.isAddressInLocalPoolForProcessor(bigPages[i], pid)) {
                            allocator.freeBigPage(bigPages[i], pid);
                            bigPages.remove(i--);
                        }
                    }
                }
            }
            tryDonatePagesIfNecessary(pid);
            hal::release_spinlock(local_locks[pid]);
        }
        assert(smallPages.getSize() == 0 && bigPages.getSize() == 0,
               "Tried to free pages that were out of allocator ranges");
    }

    // Allocates enough (physically contiguous) pages to satisfy a request for a certain capacity
    // Returns vectors of small and big pages to fulfill the request
    bool allocateContiguousPhysicalPages(size_t requestedCapacityInBytes, Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);

    // Retrieves memory usage statistics (free pages, allocated pages, distribution among local/global pools, fragmentation etc.)
    //MemoryUsageStats getUsageStatistics();
}