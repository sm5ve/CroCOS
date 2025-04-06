//
// Created by Spencer Martin on 3/9/25.
//

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
alignas(4096) uint64_t internalPageTableMapping[ENTRIES_PER_TABLE];
alignas(4096) uint64_t internalTableMetadataMapping[ENTRIES_PER_TABLE];
alignas(4096) uint64_t initialInternalPageTable[ENTRIES_PER_TABLE];
extern volatile uint64_t boot_pml4[ENTRIES_PER_TABLE];
extern volatile uint64_t boot_page_directory_pointer_table[ENTRIES_PER_TABLE];

#define GLOBAL_INFO_BIT 11
#define TABLE_LOCK_OFFSET 0
#define TABLE_HAS_SUPPLEMENTARY_TABLE GLOBAL_INFO_BIT, 1, 1
#define TABLE_ALLOCATED_COUNT GLOBAL_INFO_BIT, 10, 2
#define TABLE_SUPPLEMENT_INDEX GLOBAL_INFO_BIT, 20, 12
#define LOCAL_OFFSET_INDEX 52, 61
#define LOCAL_VIRT_ADDR 12, 31

namespace kernel::amd64::PageTableManager{

    struct PageDirectoryEntry;

    PageDirectoryEntry* pageMappingDirectory;
    PageDirectoryEntry* pageStructureVirtualBase;
    uint64_t* pageTableGlobalMetadataBase;

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

        void setAndPreserveMetadata(PageDirectoryEntry entry){
            uint64_t mask = (0xfl << 8) | (0x7ffl << 52);
            this -> value = (this -> value & mask) | (entry.value & ~mask);
        }

        template <size_t bitIndex, size_t length, size_t startEntry>
        [[nodiscard]]
        static uint64_t get_inline_global_metadata(const PageDirectoryEntry* table) {
            assert((uint64_t)table % mm::PageAllocator::smallPageSize == 0, "Page table improperly aligned");
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
        static void set_inline_global_metadata(PageDirectoryEntry* table, uint64_t value) {
            assert((uint64_t)table % mm::PageAllocator::smallPageSize == 0, "Page table improperly aligned");
            static_assert((bitIndex >= 9 && bitIndex <= 11) || (bitIndex >= 52 && bitIndex <= 58),
                          "Metadata out of bounds");
            const uint64_t mask = (1ul << bitIndex);
            for (size_t i = 0; i < length; i++) {
                const uint64_t bit = (value >> (length - i - 1)) & 1;
                table[i + startEntry] = PageDirectoryEntry((table[i + startEntry] & ~mask) | (bit << bitIndex));
            }
        }

        static uint64_t& fast_global_metadata(PageDirectoryEntry* table){
            assert((uint64_t)table % mm::PageAllocator::smallPageSize == 0, "Page table improperly aligned");
            auto absolutePageIndex = ((uint64_t)table - (uint64_t)pageStructureVirtualBase) / mm::PageAllocator::smallPageSize;
            return pageTableGlobalMetadataBase[absolutePageIndex];
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

    void freeInternalPageTableEntry(PageDirectoryEntry& entry);

    void markPageTableVaddrFree(mm::virt_addr vaddr){
        uint64_t index = (vaddr.value - (uint64_t) pageStructureVirtualBase) / mm::PageAllocator::smallPageSize;
        auto entry = reinterpret_cast<PageDirectoryEntry*>((uint64_t)pageStructureVirtualBase
                + index * sizeof(PageDirectoryEntry));
        freeInternalPageTableEntry(*entry);
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
        //if it happens that the read head was incremented but the entry is still populated, try spinning to wait
        //for the other processor to finish copying the info from the entry
        while(m.populated){
            asm volatile ("pause");
        }
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
        //if it happens that the write head was incremented but the entry is still populated, try spinning to wait
        //for the other processor to finish copying the info to the entry
        while(!m.populated){
            asm volatile ("pause");
        }
        page = m.pageInfo;
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
        //readyToProcess should be set to false before the read head is advanced, so we don't need to spin here and
        //this assert is valid
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
            while(!entry.readyToProcess){
                //So... I could try to continue, but that makes the logic for advancing the read head a lot more complicated
                //Thus, for the sake of simplicity, I will spin.
                asm volatile ("pause");
            }
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

    uint16_t unpopulatedHead;

    uint16_t partiallyOccupiedRingBuffer[ENTRIES_PER_TABLE];
    volatile uint16_t poQueueWriteHead;
    volatile uint16_t poQueueWrittenLimit;
    volatile uint16_t poQueueReadHead;
    uint64_t fullMarkers[ENTRIES_PER_TABLE / sizeof(uint64_t)];
    uint64_t partiallyOccupiedMarkers[ENTRIES_PER_TABLE / sizeof(uint64_t)];

    bool markFullState(uint16_t index, bool full){
        auto i = index / sizeof(uint64_t);
        uint64_t mask = (1 << (index % sizeof(uint64_t)));
        bool didSwap;
        do {
            auto oldValue = fullMarkers[i];
            // Check if the bit already has the desired state
            if (full == ((oldValue & mask) != 0)) {
                return false;  // No change needed
            }
            // Set or clear the bit based on 'full'
            auto newValue = full ? (oldValue | mask) : (oldValue & ~mask);
            didSwap = atomic_cmpxchg_u64(fullMarkers[i], oldValue, newValue);
        } while (!didSwap);
        return true;
    }

    bool markPartiallyOccupiedState(uint16_t index, bool partiallyOccupied){
        auto i = index / sizeof(uint64_t);
        uint64_t mask = (1 << (index % sizeof(uint64_t)));
        bool didSwap;
        do {
            auto oldValue = partiallyOccupiedMarkers[i];
            // Check if the bit already has the desired state
            if (partiallyOccupied == ((oldValue & mask) != 0)) {
                return false;  // No change needed
            }
            // Set or clear the bit based on 'partiallyOccupied'
            auto newValue = partiallyOccupied ? (oldValue | mask) : (oldValue & ~mask);
            didSwap = atomic_cmpxchg_u64(partiallyOccupiedMarkers[i], oldValue, newValue);
        } while (!didSwap);
        return true;
    }

    bool getFullState(uint16_t index){
        auto i = index / sizeof(uint64_t);
        uint64_t mask = (1 << (index % sizeof(uint64_t)));
        return (fullMarkers[i] & mask) != 0;
    }

    void initializePartiallyOccupiedRingBuffer(){
        memset(partiallyOccupiedRingBuffer, 0, sizeof(partiallyOccupiedRingBuffer));
        memset(fullMarkers, 0, sizeof(fullMarkers));
        memset(partiallyOccupiedMarkers, 0xff, sizeof(fullMarkers));
        poQueueWriteHead = 1;
        poQueueWrittenLimit = 1;
        poQueueReadHead = 0;
        unpopulatedHead = 3;
        partiallyOccupiedRingBuffer[0] = 1; //the first partially occupied page on initialization is in index 1
        markFullState(0, true); //let's mark the 0th page as full since it's special.
        markPartiallyOccupiedState(0, false); //let's mark the 0th page as not partially occupied since it's special.
    }

    void markTableAsFull(PageDirectoryEntry* table){
        auto index = static_cast<uint16_t>(((uint64_t) table - (uint64_t) pageStructureVirtualBase) /
                                               mm::PageAllocator::smallPageSize);
        if(markFullState(index, true)){
            //If we're the ones to mark the state as full, we're responsible for removing the index from the queue
            assert(partiallyOccupiedRingBuffer[poQueueReadHead] == index,
                   "Erroneously marked table as full that is not at front of queue");
            //Because we were the ones to mark the full state of the page as true, we know we are guaranteed to have
            //exclusive ownership over the read head.
            poQueueReadHead = static_cast<uint16_t>((poQueueReadHead + 1) % ENTRIES_PER_TABLE);
            markPartiallyOccupiedState(index, false);
        }
    }

    void markTableAsPartiallyOccupied(PageDirectoryEntry* table){
        auto index = static_cast<uint16_t>(((uint64_t) table - (uint64_t) pageStructureVirtualBase) /
                                               mm::PageAllocator::smallPageSize);
        if(markPartiallyOccupiedState(index, true)){
            //If we're the ones to mark the state as partially occupied, we're responsible for pushing this to the queue
            //unlike in the above case, it is expected that we may try to mark many different indices as partially occupied
            //at the same time, so we have no guarantees over exclusive ownership over the write head
            bool didAdvanceHead;
            uint16_t prevWriteHead;
            do{
                prevWriteHead = poQueueWriteHead;
                auto next = static_cast<uint16_t>((prevWriteHead + 1) % ENTRIES_PER_TABLE);
                didAdvanceHead = atomic_cmpxchg_u16(poQueueWriteHead, prevWriteHead, next);
            } while(!didAdvanceHead);
            //write the index to the queue
            partiallyOccupiedRingBuffer[prevWriteHead] = index;
            //mark the page as not full, allowing it to be marked as full again in the future
            markFullState(index, false);
            mfence();
            while(true){
                if(atomic_cmpxchg_u16(poQueueWrittenLimit, prevWriteHead, prevWriteHead + 1)){
                    break;
                }
            };
        }
    }

    PageDirectoryEntry* getPageTableForIndex(uint64_t index){
        return (PageDirectoryEntry*)((uint64_t)pageStructureVirtualBase + index * mm::PageAllocator::smallPageSize);
    }

    PageDirectoryEntry* allocateInternalPageTableEntryForTable(PageDirectoryEntry* table){
        assert(((uint64_t)table & ((1ul << 12) - 1)) == 0, "page table not aligned");
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
        assert(!table[index].present(), "Tried to allocate a page table entry that was already present");
        return &table[index];
    }

    void freeInternalPageTableEntry(PageDirectoryEntry& entry){
        auto tableBase = (PageDirectoryEntry*)((uint64_t)&entry & (uint64_t)~((1 << 12) - 1));
        uint64_t entryIndex = ((uint64_t)&entry - (uint64_t)tableBase) / sizeof(PageDirectoryEntry);
        bool didFree;
        do{
            auto priorHead = tableBase[0];
            entry = PageDirectoryEntry(0);
            entry.set_local_metadata<LOCAL_VIRT_ADDR, true>(priorHead.get_local_metadata<LOCAL_OFFSET_INDEX>());
            auto newHead = priorHead;
            newHead.set_local_metadata<LOCAL_OFFSET_INDEX>(entryIndex);
            didFree = atomic_cmpxchg_u64(tableBase[0].value, priorHead.value, newHead.value);
        } while(!didFree);
        //Mark the table as partially occupied - if it used to be full, move it to the queue
        markTableAsPartiallyOccupied(tableBase);
    }

    void allocateNewPageTableIfNecessary(){
        auto prevWriteHead = poQueueWriteHead;
        //check if the queue is empty
        if(poQueueReadHead == prevWriteHead){
            auto nextWriteHead = static_cast<uint16_t>((prevWriteHead + 1) % ENTRIES_PER_TABLE);
            //If we can advance the write head, we're responsible for allocating a new page table and adding it to the queue!
            if(atomic_cmpxchg_u16(poQueueWriteHead, prevWriteHead, nextWriteHead)){
                //TODO for the fast global metadata, I will have to allocate and map in a second page here...
                //In particular, only one processor at a time can possibly be running this code
                auto backingPageAddr = mm::PageAllocator::allocateSmallPage();
                //Map the table into memory with R/W and as global
                internalPageTableMapping[unpopulatedHead] = PageDirectoryEntry(backingPageAddr.value | (1 << 8) | 3);
                //Map in a new global metadata page
                auto metadataPageAddr = mm::PageAllocator::allocateSmallPage();
                internalTableMetadataMapping[unpopulatedHead] = PageDirectoryEntry(metadataPageAddr.value | (1 << 8) | 3);
                //clear the page table and its metadata
                memset((void*)((uint64_t)pageStructureVirtualBase + unpopulatedHead * mm::PageAllocator::smallPageSize), 0, mm::PageAllocator::smallPageSize);
                memset((void*)((uint64_t)pageTableGlobalMetadataBase + unpopulatedHead * mm::PageAllocator::smallPageSize), 0, mm::PageAllocator::smallPageSize);
                //initialize the page table
                initializeInternalPageTableFreeMetadata(getPageTableForIndex(unpopulatedHead));
                //put it in the queue
                partiallyOccupiedRingBuffer[prevWriteHead] = unpopulatedHead;
                //install the page table in the internal page directory
                pageMappingDirectory[unpopulatedHead] = PageDirectoryEntry(backingPageAddr.value | (1 << 8) | 3);
                //advance the unpopulated head
                unpopulatedHead++;
                //finally advance the written limit
                mfence();
                while(true){
                    if(atomic_cmpxchg_u16(poQueueWrittenLimit, prevWriteHead, nextWriteHead)){
                        break;
                    }
                }
            }
        }
    }

    PageDirectoryEntry* allocateInternalPageTableEntry(){
        while(true){
            //if the queue is not empty, try to allocate a page from the table pointed to by the read head
            auto readHead = poQueueReadHead;
            if(readHead != poQueueWrittenLimit){
                //double-check that the entry is not marked as full
                if(!getFullState(partiallyOccupiedRingBuffer[readHead])){
                    //try to allocate an entry
                    auto table = getPageTableForIndex(partiallyOccupiedRingBuffer[readHead]);
                    auto out = allocateInternalPageTableEntryForTable(table);
                    if(out != nullptr){
                        return out;
                    }
                    //if the allocation failed, mark the table as full
                    markTableAsFull(table);
                }
            }
            else{
                //if the queue is empty (or there are writes to the queue still pending) allocate a new page table
                allocateNewPageTableIfNecessary();
                //this is a tight loop, so this should help on hyperthreaded cpus
                asm volatile("pause");
            }
        }
    }

    void runSillyTest(){
        kernel::DbgOut << "allocating page table entry at " << allocateInternalPageTableEntry() << "\n";
        PageDirectoryEntry* entries[3000];
        for(auto i = 0; i < 3000; i++){
            entries[i] = allocateInternalPageTableEntry();
        }
        for(auto i = 0; i < 3000; i++){
            allocateInternalPageTableEntry();
            freeInternalPageTableEntry(*entries[i]);
        }
        kernel::DbgOut << "allocating page table entry at " << allocateInternalPageTableEntry() << "\n";
        kernel::DbgOut << "poQueueReadHead is " << poQueueReadHead << "\n";
        kernel::DbgOut << "poQueueWrittenLimit is " << poQueueWrittenLimit << "\n";
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
        memset(internalPageTableMapping, 0, sizeof(internalPageTableMapping));
        memset(internalTableMetadataMapping, 0, sizeof(internalTableMetadataMapping));
        memset(initialInternalPageTable, 0, sizeof(initialInternalPageTable));
        //Initialize the free list metadata
        initializeInternalPageTableFreeMetadata((PageDirectoryEntry*)internalPageTableMapping);
        initializeInternalPageTableFreeMetadata((PageDirectoryEntry*)initialInternalPageTable);

        //Initialize the metadata for the internal page directory - this is done by hand
        initializePartiallyOccupiedRingBuffer();
        //Allocate the page table entries to map in our two startup page tables and map the page directory 2 MiB into our window
        auto entryInMappingPT_for_PTMap= allocateInternalPageTableEntryForTable((PageDirectoryEntry*)internalPageTableMapping);
        auto entryInMappingPT_for_PTMetadataMap= allocateInternalPageTableEntryForTable((PageDirectoryEntry*)internalPageTableMapping);
        auto entryInMappingPT_for_initialPT= allocateInternalPageTableEntryForTable((PageDirectoryEntry*)internalPageTableMapping);
        auto entryInInitialPT_for_PDir = allocateInternalPageTableEntryForTable((PageDirectoryEntry*)initialInternalPageTable);

        //Populate the entries by hand
        entryInMappingPT_for_PTMap -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(internalPageTableMapping)).value | 3 | (1 << 8)));
        entryInMappingPT_for_PTMetadataMap -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(internalPageTableMapping)).value | 3 | (1 << 8)));
        entryInMappingPT_for_initialPT -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(initialInternalPageTable)).value | 3 | (1 << 8)));
        entryInInitialPT_for_PDir -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(bootstrapPageDir)).value | 3 | (1 << 8)));

        //Set up the initial state of the page directory - internalPageTableMapping goes in the bottom, then the metadata mapping,
        //and finally initialInternalPageTable which maps in the page directory and acts as our first general purpose internal page table
        bootstrapPageDir[0] = amd64::early_boot_virt_to_phys(mm::virt_addr(internalPageTableMapping)).value | 3;
        bootstrapPageDir[1] = amd64::early_boot_virt_to_phys(mm::virt_addr(internalTableMetadataMapping)).value | 3;
        bootstrapPageDir[2] = amd64::early_boot_virt_to_phys(mm::virt_addr(initialInternalPageTable)).value | 3;

        //Set up our initial metadata page
        for(auto i = 0; i < 3; i++){
            internalTableMetadataMapping[i] = PageDirectoryEntry(mm::PageAllocator::allocateSmallPage().value | 3 | (1 << 8));
        }

        //set up our pointers
        pageStructureVirtualBase = reinterpret_cast<PageDirectoryEntry *>(-3ul << 30); //-3 GiB
        pageTableGlobalMetadataBase = reinterpret_cast<uint64_t *>((uint64_t)pageStructureVirtualBase + (1ul << 21));
        pageMappingDirectory = (PageDirectoryEntry*)((uint64_t)pageTableGlobalMetadataBase + (1ul << 21));

        //install the page directory
        boot_page_directory_pointer_table[509] = amd64::early_boot_virt_to_phys(mm::virt_addr(bootstrapPageDir)).value | 3;

        //Now that the directory's mapped in where we expect it to be, initialize the metadata!
        initializeInternalPageDirectoryFreeMetadata(pageMappingDirectory);
        memset(pageTableGlobalMetadataBase, 0, mm::PageAllocator::smallPageSize * 3);
        //ensureReservePoolNotEmpty();

        runSillyTest();
    }
}