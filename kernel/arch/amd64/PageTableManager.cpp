//
// Created by Spencer Martin on 3/9/25.
//

#include "PageTableManager.h"
#include "kernel.h"
#include <arch/hal.h>
#include <arch/amd64.h>
#include <mm.h>
#include <lib/math.h>
#include <utility.h>

//I just set these constants arbitrarily. They feel right, but I should experiment with other values at some point
#define FREE_OVERFLOW_POOL_SIZE 128

#define RESERVE_POOL_SIZE 128
#define RESERVE_POOL_DEFAULT_FILL 48
#define RESERVE_POOL_LAZY_FILL_THRESHOLD 16

#define BULK_FREE_POOL_SIZE 1024

#define ENTRIES_PER_TABLE 512

extern uint64_t bootstrapPageDir[ENTRIES_PER_TABLE];
alignas(4096) uint64_t pageTableMappingTable[ENTRIES_PER_TABLE];
alignas(4096) uint64_t pageTable0[ENTRIES_PER_TABLE];
extern volatile uint64_t boot_pml4[ENTRIES_PER_TABLE];
extern volatile uint64_t boot_page_directory_pointer_table[ENTRIES_PER_TABLE];

#define GLOBAL_INFO_BIT 11
#define TABLE_LOCK_OFFSET 0
#define TABLE_HAS_SUPPLEMENTARY_TABLE GLOBAL_INFO_BIT, 1, 1
#define TABLE_ALLOCATED_COUNT GLOBAL_INFO_BIT, 10, 2
#define TABLE_SUPPLEMENT_OFFSET GLOBAL_INFO_BIT, 20, 12
#define LOCAL_OFFSET_INDEX 52, 61
#define LOCAL_VIRT_ADDR 12, 31
#define LOCAL_INTERNAL_TABLE_MARKED_FULL 10, 10

namespace kernel::amd64::PageTableManager{

    struct PageDirectoryEntry;

    PageDirectoryEntry* pageMappingDirectory;
    PageDirectoryEntry* pageStructureVirtualBase;

    // PageDirectoryEntry is a trivial wrapper around uint64_t.
    struct alignas(8) PageDirectoryEntry {
        uint64_t value = 0;  // Raw page table entry.

        constexpr PageDirectoryEntry() = default;
        constexpr explicit PageDirectoryEntry(uint64_t raw) : value(raw) {}

        constexpr operator uint64_t() const { return value; }

        template <size_t start, size_t end, bool freeEntry = false>
        [[nodiscard]]
        constexpr uint64_t get_local_metadata() const {
            //Make sure we're only using bits marked "available" by the x86 paging spec
            //This check is only valid for entries pointing to other tables. This is notably invalid for big pages
            static_assert((!freeEntry && ((start >= 8 && end <= 11) || (start >= 52 && end <= 62) || (start == 6 && end == 6))) || (freeEntry && (start >= 1 && end <= 62)),
                          "Metadata out of bounds");
            return (value >> start) & ((1ul << (end - start + 1)) - 1);
        }

        template <size_t start, size_t end, bool freeEntry = false>
        void set_local_metadata(uint64_t metadata) {
            //Make sure we're only using bits marked "available" by the x86 paging spec
            //This check is only valid for entries pointing to other tables. This is notably invalid for big pages
            static_assert((!freeEntry && ((start >= 8 && end <= 11) || (start >= 52 && end <= 62) || (start == 6 && end == 6))) || (freeEntry && (start >= 1 && end <= 62)),
                          "Metadata out of bounds");
            uint64_t mask = ((1ul << (end - start + 1)) - 1) << start;
            value = (value & ~mask) | ((metadata & ((1ul << (end - start + 1)) - 1)) << start);
        }

        [[nodiscard]]
        constexpr bool was_accessed() const{
            return (value >> 5) & 1;
        }

        [[nodiscard]]
        constexpr bool present() const{
            return value & 1;
        }

        template <size_t bitIndex, size_t length, size_t startEntry>
        [[nodiscard]]
        static uint64_t get_global_metadata(const PageDirectoryEntry* table) {
            //global metadata should support both big page entries and entries mapping to page tables, hence the
            //more restrictive assert.
            static_assert((bitIndex >= 9 && bitIndex <= 11) || (bitIndex >= 52 && bitIndex <= 58),
                          "Metadata out of bounds");
            uint64_t out = 0;
            for (size_t i = 0; i < length; i++) {
                out = (out << 1) | ((table[i + startEntry] >> bitIndex) & 1);
            }
            return out;
        }

        template <size_t bitIndex, size_t length, size_t startEntry>
        static void set_global_metadata(PageDirectoryEntry* table, uint64_t value) {
            static_assert((bitIndex >= 9 && bitIndex <= 11) || (bitIndex >= 52 && bitIndex <= 58),
                          "Metadata out of bounds");
            const uint64_t mask = (1ul << bitIndex);
            for (size_t i = 0; i < length; i++) {
                const uint64_t bit = (value >> (length - i - 1)) & 1;
                table[i + startEntry] = PageDirectoryEntry((table[i + startEntry] & ~mask) | (bit << bitIndex));
            }
        }

        static void acquire_table_lock(PageDirectoryEntry* table){
            bool acquired_lock;
            do{
                PageDirectoryEntry previousEntry = table[TABLE_LOCK_OFFSET];
                while(previousEntry.get_local_metadata<GLOBAL_INFO_BIT, GLOBAL_INFO_BIT>()){
                    asm volatile("pause");
                    previousEntry = table[TABLE_LOCK_OFFSET];
                };
                PageDirectoryEntry lockedEntry = previousEntry;
                lockedEntry.set_local_metadata<GLOBAL_INFO_BIT, GLOBAL_INFO_BIT>(true);
                acquired_lock = amd64::atomic_cmpxchg_u64(table[TABLE_LOCK_OFFSET].value,
                                                          previousEntry.value,
                                                          lockedEntry.value);
            } while(!acquired_lock);
        }

        static void release_table_lock(PageDirectoryEntry* table){
            table[TABLE_LOCK_OFFSET].set_local_metadata<GLOBAL_INFO_BIT, GLOBAL_INFO_BIT>(false);
        }
    };

    static_assert(sizeof(PageDirectoryEntry) == 8, "PageDirectoryEntry of wrong size");
    static_assert(sizeof(PageDirectoryEntry[512]) == 4096, "PageDirectoryEntry improperly packed");

    uint64_t toProcessBitmapBlank[divideAndRoundUp(kernel::hal::MAX_PROCESSOR_COUNT,sizeof(uint64_t))];
    size_t meaningfulBitmapPages;

    struct PageInfo{
        kernel::mm::phys_addr physicalAddress;
        kernel::mm::virt_addr virtualAddress;

        constexpr PageInfo() = default;
    };

    struct ReservePoolEntry{
        PageInfo pageInfo;
        bool populated;
    };

    struct OverflowPoolEntry{
        PageInfo pageInfo;
        bool readyToProcess;
        uint64_t toProcessBitmap[kernel::hal::MAX_PROCESSOR_COUNT / sizeof(uint64_t)];

        constexpr OverflowPoolEntry() = default;

        OverflowPoolEntry& operator=(const OverflowPoolEntry& other) = default;
    };

    ReservePoolEntry reservePool[RESERVE_POOL_SIZE];
    volatile uint64_t reservePoolWriteHead = 0;
    volatile uint64_t reservePoolReadHead = 0;

    OverflowPoolEntry freeOverflowPool[FREE_OVERFLOW_POOL_SIZE];
    volatile uint64_t freeOverflowWriteHead = 0;
    volatile uint64_t freeOverflowReadHead = 0;

    void markPageTableVaddrFree(mm::virt_addr vaddr){
        (void)vaddr;
        //TODO implement
    }

    bool addPageToReservePool(PageInfo& page){
        bool incrementedWriteHead = false;
        uint64_t prevValue;
        //Try to advance the write head atomically.
        do{
            prevValue = reservePoolWriteHead;
            uint64_t nextValue = (prevValue + 1) % RESERVE_POOL_SIZE;
            if(nextValue == reservePoolReadHead){
                //If the ring buffer's full, abort!!!
                return false;
            }
            incrementedWriteHead = kernel::amd64::atomic_cmpxchg_u64(reservePoolWriteHead, prevValue, nextValue);
        } while(!incrementedWriteHead);
        ReservePoolEntry& m = reservePool[prevValue];
        assert(!m.populated, "Reserve pool ring buffer state is insane!");
        m.pageInfo = page;
        kernel::amd64::mfence();
        m.populated = true;
        return true;
    }

    bool readPageFromReservePool(PageInfo& page){
        bool incrementedReadHead = false;
        uint64_t prevValue;
        //Try to advance the read head atomically.
        do{
            prevValue = reservePoolReadHead;
            uint64_t nextValue = (prevValue + 1) % RESERVE_POOL_SIZE;
            if(prevValue == reservePoolWriteHead){
                //If the ring buffer's empty, abort!!!
                return false;
            }
            incrementedReadHead = kernel::amd64::atomic_cmpxchg_u64(reservePoolReadHead, prevValue, nextValue);
        } while(!incrementedReadHead);
        ReservePoolEntry& m = reservePool[prevValue];
        assert(!m.populated, "Reserve pool ring buffer state is insane!");
        m.pageInfo = page;
        kernel::amd64::mfence();
        m.populated = false;
        return true;
    }

    bool addPageToOverflowPool(PageInfo& page){
        bool incrementedWriteHead = false;
        uint64_t prevValue;
        //Try to advance the write head atomically.
        do{
            prevValue = freeOverflowWriteHead;
            uint64_t nextValue = (prevValue + 1) % FREE_OVERFLOW_POOL_SIZE;
            if(nextValue == freeOverflowReadHead){
                //If the ring buffer's full, abort!!!
                return false;
            }
            incrementedWriteHead = kernel::amd64::atomic_cmpxchg_u64(freeOverflowWriteHead, prevValue, nextValue);
        } while(!incrementedWriteHead);
        OverflowPoolEntry& m = freeOverflowPool[prevValue];
        assert(!m.readyToProcess, "Free overflow ring buffer state is insane!");
        m.pageInfo = page;
        memcpy(m.toProcessBitmap, toProcessBitmapBlank, sizeof(toProcessBitmapBlank));
        kernel::amd64::mfence();
        m.readyToProcess = true;
        return true;
    }

    void processOverflowPool(){
        auto pid = hal::getCurrentProcessorID();
        auto processorInd = divideAndRoundDown((size_t)pid, sizeof(uint64_t));
        auto mask = (1ul << (pid % sizeof(uint64_t)));
        for(auto index = freeOverflowReadHead; index != freeOverflowWriteHead; index = (index + 1) % FREE_OVERFLOW_POOL_SIZE){
            auto& entry = freeOverflowPool[index];
            if(!entry.readyToProcess)
                //So... I could try to continue, but that makes the logic for advancing the read head a lot more complicated
                //Thus, for the sake of simplicity, I will break. I cannot imagine a situation where one processor is in the
                //midst of writing a single entry to the ring buffer while the buffer is filled with a large number of
                //other entries
                break;
            if(!(entry.toProcessBitmap[processorInd] & mask))
                continue;
            //If we haven't processed this entry in the freeOverflowPool yet, flush the TLB entry for the corresponding page!
            kernel::amd64::invlpg(entry.pageInfo.virtualAddress.value);
            //Then flip the bit indicating we've processed this TLB flush
            kernel::amd64::atomic_and(entry.toProcessBitmap[processorInd], ~mask);
            kernel::hal::compiler_fence();
            //If all other processors have processed this page, increment the freeOverflowReadHead!
            size_t clearBitmaps = 0;
            for(size_t i = 0; i < meaningfulBitmapPages; i++){
                if(entry.toProcessBitmap[i] == 0){
                    clearBitmaps++;
                }
            }
            if(clearBitmaps == meaningfulBitmapPages){
                //This makes sure that we don't increment the freeOverflowReadHead twice if two processors simultaneously
                //determine that all others have completed their TLB flushes
                auto pageAddress = entry.pageInfo.physicalAddress;
                auto virtAddress = entry.pageInfo.virtualAddress;
                //No need to process this anymore!
                kernel::amd64::mfence();
                entry.readyToProcess = false;
                auto didIncrementReadHead = kernel::amd64::atomic_cmpxchg_u64(freeOverflowReadHead, index, index + 1);
                //If we were the ones to increment the read head, then we can release the page back to the page allocator
                //without fear of a double-free!
                if(didIncrementReadHead) {
                    kernel::mm::PageAllocator::freeSmallPage(pageAddress);
                    markPageTableVaddrFree(virtAddress);
                }
            }
        }
    }

    //Initializes the metadata in the page table entries to encode a linked list allowing O(1) free virtual address
    //discovery. The free metadata makes use of 2 primary "global" table variables, and one local metadata entry per free
    //entry.
    //
    //The first is a counter for the number of *present* entries. Note that this is not the same as
    //(512 - free entries) as certain entries may not be present, but may still be used to store virtual
    //address metadata for linked tables. However, the number of present entries will either be (512 - free entries)
    //or half this quantity, depending on a bit indicating the absence or presence of a supplementary virtual address
    //table.
    //
    //The second global variable is the index of the first "free" entry. This is the head of the linked list of free
    //entries in our table.
    //
    //Note that this is only to be used internally by the page table manager – in the broader kernel, the higher level
    //abstraction of VirtualMemoryZones will solve virtual address allocation using an augmented AVL tree.
    void initializeInternalPageTableFreeMetadata(PageDirectoryEntry* table){
        for(uint64_t i = 0; i < ENTRIES_PER_TABLE; i++){
            //We will say any offset with the highest bit set is invalid - in particular this enforces that there is
            //no "next" free entry after the last one, and this is recorded by the fact that the next entry offset
            //in the last entry is 512, which has bit 9 set.
            table[i].set_local_metadata<LOCAL_VIRT_ADDR, true>(i + 1);
        }
        table[0].set_local_metadata<LOCAL_OFFSET_INDEX>(0);
    }

    void initializeInternalPageDirectoryFreeMetadata(PageDirectoryEntry* table){
        for(uint64_t i = 0; i < ENTRIES_PER_TABLE; i++){
            //We will say any offset with the highest bit set is invalid - in particular this enforces that there is
            //no "next" free entry after the last one, and this is recorded by the fact that the next entry offset
            //in the last entry is 512, which has bit 9 set.
            table[i].set_local_metadata<LOCAL_OFFSET_INDEX>(i + 1);
        }
    }

    uint64_t partiallyAllocatedLinkedListHeadIndex;
    uint64_t unpopulatedHead;
    hal::spinlock_t directoryLinkedListLock;

    PageDirectoryEntry* getPageTableForIndex(uint64_t index){
        return (PageDirectoryEntry*)((uint64_t)pageStructureVirtualBase + index * mm::PageAllocator::smallPageSize);
    }

    PageDirectoryEntry* allocateInternalPageTableEntryForTable(PageDirectoryEntry* table){
        bool didAllocate;
        uint64_t index;
        do{
            PageDirectoryEntry prevEntry = table[0];
            index = prevEntry.get_local_metadata<LOCAL_OFFSET_INDEX>();
            if(index >= ENTRIES_PER_TABLE){
                //FIXME this should be like... an ErrorOr sort of thing. This isn't technically incorrect
                return nullptr;
            }
            PageDirectoryEntry newEntry = prevEntry;
            uint64_t nextIndex = table[index].get_local_metadata<LOCAL_VIRT_ADDR, true>();
            newEntry.set_local_metadata<LOCAL_OFFSET_INDEX>(nextIndex);
            didAllocate = atomic_cmpxchg_u64(table[0].value, prevEntry.value, newEntry.value);
        } while(!didAllocate);
        return &table[index];
    }

    PageDirectoryEntry* allocateInternalPageTableEntry(){
        while(true){
            //If there are no partially allocated pages in the linked list, allocate a new page and map it in
            auto prevUnpopulatedHead = unpopulatedHead;
            if(partiallyAllocatedLinkedListHeadIndex == prevUnpopulatedHead){
                assert(prevUnpopulatedHead < ENTRIES_PER_TABLE, "The page table manager ran out of space");
                //if we are able to increment the unpopulated head, then we are responsible for allocating a page
                //and mapping it in to virtual memory
                if(atomic_cmpxchg_u64(unpopulatedHead, prevUnpopulatedHead, prevUnpopulatedHead + 1)){
                    //allocate the backing memory
                    auto physPage = mm::PageAllocator::allocateSmallPage();
                    //map it into virtual address space as a global page with R/W privileges
                    pageTableMappingTable[prevUnpopulatedHead] = physPage.value | 3 | (1 << 8);
                    //now initialize the metadata for the free list
                    initializeInternalPageTableFreeMetadata(getPageTableForIndex(prevUnpopulatedHead));
                    //Add in a memory fence, since we're going to use the value of pageMappingDirectory[prevUnpopulatedHead]
                    //to determine when the page table is ready to use
                    mfence();
                    //finally map it as a page table in the page directory
                    auto next = PageDirectoryEntry(physPage.value | 3);
                    next.set_local_metadata<LOCAL_OFFSET_INDEX>(pageMappingDirectory[prevUnpopulatedHead].get_local_metadata<LOCAL_OFFSET_INDEX>());
                    pageMappingDirectory[prevUnpopulatedHead] = next;
                }
            }
            //If we happened to make it here while another processor is still populating this entry, wait!
            while(!pageMappingDirectory[partiallyAllocatedLinkedListHeadIndex].present()){
                asm volatile("pause");
            }
            auto priorListHead = partiallyAllocatedLinkedListHeadIndex;
            auto potentialOut = allocateInternalPageTableEntryForTable(getPageTableForIndex(priorListHead));
            if(potentialOut != nullptr){
                return potentialOut;
            }
            //if the page table seems to be full, try to remove it from the partially allocated linked list
            hal::acquire_spinlock(directoryLinkedListLock);
            //verify no other processor has advanced the linked list head
            if(priorListHead == partiallyAllocatedLinkedListHeadIndex){
                //advance the head
                partiallyAllocatedLinkedListHeadIndex = pageMappingDirectory[priorListHead].get_local_metadata<LOCAL_OFFSET_INDEX>();

                //mark the table as full - this data will be used later when freeing a page table to determine if the
                //internal page table should be added back to the linked list
                pageMappingDirectory[priorListHead].set_local_metadata<LOCAL_INTERNAL_TABLE_MARKED_FULL>(true);
            }
            hal::release_spinlock(directoryLinkedListLock);
        }
    }

    void init(size_t processorCount){
        auto index = 0;
        meaningfulBitmapPages = divideAndRoundUp(processorCount, sizeof(uint64_t));
        auto count = processorCount;
        while(count > 0){
            if(count >= 64){
                toProcessBitmapBlank[index] = (uint64_t) -1;
                count -= 64;
            }
            else{
                toProcessBitmapBlank[index] = (1ul << count) - 1;
                count = 0;
            }
            index++;
        }
        //In order to actually manage page tables and directories, the page table manager needs to be able to map pages
        //into virtual address space. It's a kind of chicken-and-egg problem, which we resolve by setting up the initial
        //structures by hand. In particular, we are setting aside the 1 GiB span of virtual address space at -3 GiB
        //for the page table manager to map in page tables that are in use.
        memset(bootstrapPageDir, 0, sizeof(bootstrapPageDir));
        memset(pageTableMappingTable, 0, sizeof(pageTableMappingTable));
        memset(pageTable0, 0, sizeof(pageTable0));
        //Initialize the free list metadata
        initializeInternalPageTableFreeMetadata((PageDirectoryEntry*)pageTableMappingTable);
        initializeInternalPageTableFreeMetadata((PageDirectoryEntry*)pageTable0);
        //Initialize the metadata for the internal page directory - this is done by hand
        unpopulatedHead = 2;
        partiallyAllocatedLinkedListHeadIndex = 1;
        //Allocate the page table entries to map in our two startup page tables and map the page directory 2 MiB into our window
        auto ptmt0 = allocateInternalPageTableEntryForTable((PageDirectoryEntry*)pageTableMappingTable);
        auto ptmt1 = allocateInternalPageTableEntryForTable((PageDirectoryEntry*)pageTableMappingTable);
        auto pdirEntry = allocateInternalPageTableEntryForTable((PageDirectoryEntry*)pageTable0);

        //Populate the entries by hand
        *ptmt0 = PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(pageTableMappingTable)).value | 3 | (1 << 8));
        *ptmt1 = PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(pageTable0)).value | 3 | (1 << 8));
        *pdirEntry = PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(bootstrapPageDir)).value | 3 | (1 << 8));

        //Set up the initial state of the page directory - pageTableMappingTable goes in the bottom, and pageTable0 goes next, and serves
        //as our first general purpose internal page table
        bootstrapPageDir[0] = amd64::early_boot_virt_to_phys(mm::virt_addr(pageTableMappingTable)).value | 3;
        bootstrapPageDir[1] = amd64::early_boot_virt_to_phys(mm::virt_addr(pageTable0)).value | 3;

        //set up our pointers
        pageStructureVirtualBase = reinterpret_cast<PageDirectoryEntry *>(-3ul << 30); //-3 GiB
        pageMappingDirectory = (PageDirectoryEntry*)((uint64_t)pageStructureVirtualBase + (1ul << 21));

        //install the page directory
        boot_page_directory_pointer_table[509] = amd64::early_boot_virt_to_phys(mm::virt_addr(bootstrapPageDir)).value | 3;

        //Now that the directory's mapped in where we expect it to be, initialize the metadata!
        initializeInternalPageDirectoryFreeMetadata(pageMappingDirectory);
    }
}