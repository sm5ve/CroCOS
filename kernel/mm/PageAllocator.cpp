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

        inline BigPageIndex& bigPageRefFromFreeMap(BigPageIndex i){
            auto freeMapping = bigPageFreeMapEntry(i);
            return bigPageAtIndex(freeMapping.index, bigPagePoolForBufferId(freeMapping.bufferId));
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
            swapBigPages(i, localPool[bottomOfFreePool]);
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
            swapBigPages(i, localPool[bottomOfUsedPool]);
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
            BigPageIndex& bottomUsed = localPool[bottomOfUsedPool];
            BigPageIndex& bottomFree = localPool[bottomOfFreePool];
            rotateLeft(bigPageFreeMapEntry(bottomUsed), bigPageFreeMapEntry(bottomFree), bigPageFreeMapEntry(i));
            rotateLeft(bottomUsed, bottomFree, i);
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
            //First check if the big page belongs to the global pool, and if so, claim ownership of it
            if(bigPageFreeMapEntry(bi).bufferId == GLOBAL_POOL){
                claimSpecificBigPageFromGlobalPool(bi, pid);
            }
#ifdef ALLOCATOR_DEBUG
            assert(bigPageFreeMapEntry(bi).bufferId == fromProcID(pid), "Tried to reserve small page from wrong pool");
            assert((size_t) bigPageFreeMapEntry(bi).index >= local_pool_info[pid].bottom_of_free_pool,
                   "Tried to reserve big page that's already in use");
#endif
            moveFreeBigPageToAllocated(bigPageRefFromFreeMap(bi), bigPagePoolForProcessor(pid), local_pool_info[pid].bottom_of_free_pool);
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

            //Allocate an array for the local pool info in the "heap" (bump allocator for now), initialize
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
                //for every big page, initialize the corresponding part of the small page pool
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

    // Allocates a small page and returns its physical address
    phys_addr allocateSmallPage();

    // Allocates a big page and returns its physical address
    phys_addr allocateBigPage();

    // Frees a small page that was allocated to the local processor's pool
    void freeLocalSmallPage(phys_addr page);

    // Frees a big page that was allocated to the local processor's pool
    void freeLocalBigPage(phys_addr page);

    // Frees a small page (with no restriction on the which pool the page belongs to)
    void freeSmallPage(phys_addr page);

    // Frees a big page (with no restriction on the which pool the page belongs to)
    void freeBigPage(phys_addr page);

    // Allocates enough pages to satisfy a request for a certain capacity
    // Returns vectors of small and big pages to fulfill the request
    bool allocatePages(size_t capacity, Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);

    // Frees a set of pages that were allocated to the local processor's pool
    void freeLocalPages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);

    // Frees a set of pages (with no restriction on the which pools the pages belong to)
    void freePages(Vector<phys_addr>& smallPages, Vector<phys_addr>& bigPages);

    // Retrieves memory usage statistics (free pages, allocated pages, distribution among local/global pools, fragmentation etc.)
    //MemoryUsageStats getUsageStatistics();
}