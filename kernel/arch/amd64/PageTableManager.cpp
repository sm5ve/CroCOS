//
// Created by Spencer Martin on 3/9/25.
//

#include "kernel.h"
#include "arch/hal/hal.h"
#include "arch/amd64/amd64.h"
#include <mm.h>
#include <assert.h>
#include <core/math.h>
#include <core/utility.h>
#include <core/atomic.h>

//I just set these constants arbitrarily. They feel right, but I should experiment with other values at some point
#define FREE_OVERFLOW_POOL_SIZE 128

#define RESERVE_POOL_SIZE 128
#define RESERVE_POOL_DEFAULT_FILL 48
#define RESERVE_POOL_LAZY_FILL_THRESHOLD 16

//Unused for now
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
#define LOCAL_OFFSET_INDEX 52, 61
#define LOCAL_VIRT_ADDR 12, 31
#define LOCAL_OCCUPIED_BIT 9, 9

namespace kernel::amd64::PageTableManager{

    struct PageDirectoryEntry;
    struct PTMetadata;

    PageDirectoryEntry* pageMappingDirectory;
    PageDirectoryEntry* pageStructureVirtualBase;
    uint64_t* pageTableGlobalMetadataBase;

    mm::FlushPlanner** flushPlanners;

    bool singleProcessorMode;

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
            //This check is only valid for entryBuffer pointing to other tables. This is notably invalid for big pages
            static_assert((!freeEntry && ((start >= 8 && end <= 11) || (start >= 52 && end <= 62) || (start == 6 && end == 6))) || (freeEntry && (start >= 1 && end <= 62)),
                          "Metadata out of bounds");
            return (value >> start) & ((1ul << (end - start + 1)) - 1);
        }

        template <size_t start, size_t end, bool freeEntry = false>
        void set_local_metadata(uint64_t metadata) {
            //Make sure we're only using bits marked "available" by the x86 paging spec
            //This check is only valid for entryBuffer pointing to other tables. This is notably invalid for big pages
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

        void setPresentState(bool present){
            value = (value & ~(1ul)) | (present ? 1 : 0);
        }

        void setAndPreserveMetadata(PageDirectoryEntry entry){
            uint64_t mask = (0xfl << 8) | (0x7ffl << 52);
            this -> value = (this -> value & mask) | (entry.value & ~mask);
        }

        template <size_t bitIndex, size_t length, size_t startEntry>
        [[nodiscard]]
        static uint64_t get_inline_global_metadata(const PageDirectoryEntry* table) {
            assert((uint64_t)table % mm::PageAllocator::smallPageSize == 0, "Page table improperly aligned");
            //global metadata should support both big page entryBuffer and entryBuffer mapping to page tables, hence the
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

        static PTMetadata& fast_global_metadata(PageDirectoryEntry* table){
            assert((uint64_t)table % mm::PageAllocator::smallPageSize == 0, "misaligned page table");
            auto absolutePageIndex = ((uint64_t)table - (uint64_t)pageStructureVirtualBase) / mm::PageAllocator::smallPageSize;
            return (PTMetadata&)pageTableGlobalMetadataBase[absolutePageIndex];
        }
    };

    struct alignas(8) PTMetadata{
        uint64_t value = 0;  // Raw metadata entry.

        static const uint64_t writerLockRequestMask = 0b10;
        static const uint64_t writerLockHeldMask = 0b01;
        static const uint64_t writerLockMask = writerLockRequestMask | writerLockHeldMask;
        static const uint64_t readerLockMask = 0b111111 << 2;

        void acquire_table_reader_lock() {
            // Spin until no writer is holding the acquire and no writer is waiting
            while (true) {
                // Wait for writer acquire to be released (0b00)
                while ((value & writerLockMask) != 0) {
                    asm volatile("pause");
                }

                uint64_t count;

                // Check if we have space for more readers (reader count < 63)
                while ((count = ((value & readerLockMask) >> 2)) == (readerLockMask >> 2)) {
                    asm volatile("pause");
                }

                // Prepare the new metadata with incremented reader count
                uint64_t maskedMetadata = value & ~(writerLockMask | readerLockMask);
                uint64_t expectedOldMetadata = maskedMetadata | (count << 2);
                uint64_t newMetadata = maskedMetadata | ((count + 1) << 2);

                // Try to acquire the acquire with atomic compare and exchange
                if (atomic_cmpxchg(value, expectedOldMetadata, newMetadata)) {
                    return; // Reader successfully acquired the acquire
                }
            }
        }

        void release_table_reader_lock() {
            // Continuously try to release the acquire until successful
            while (true) {
                uint64_t oldMetadata = value;
                uint64_t count = (oldMetadata & readerLockMask) >> 2;

                // Ensure that there are readers holding the acquire before releasing it
                assert(count > 0, "Tried to release reader lock when no reader held acquire");

                // Decrement the reader count
                count--;

                // Prepare the new metadata with the decremented reader count
                uint64_t newMetadata = (value & ~readerLockMask) | (count << 2);

                // Try to release the acquire with atomic compare and exchange
                if (atomic_cmpxchg(value, oldMetadata, newMetadata)) {
                    return; // Reader acquire successfully released
                }
            }
        }

        void acquire_table_writer_lock() {
            while (true) {
                // Wait until no one else is requesting the writer acquire
                while (value & writerLockRequestMask) {
                    asm volatile("pause");
                }

                uint64_t oldValue = value;
                uint64_t expectedNoRequest = oldValue & ~writerLockRequestMask;
                uint64_t requestSet = expectedNoRequest | writerLockRequestMask;

                if (atomic_cmpxchg(value, expectedNoRequest, requestSet)) {
                    // We've successfully set the request bit
                    while (true) {
                        // Wait for readers and writers to clear
                        while ((value & (readerLockMask | writerLockHeldMask)) != 0) {
                            asm volatile("pause");
                        }

                        uint64_t current = value;
                        uint64_t base = current & ~(readerLockMask | writerLockMask);

                        uint64_t expected = base | writerLockRequestMask;
                        uint64_t acquired = base | writerLockHeldMask;

                        if (atomic_cmpxchg(value, expected, acquired)) {
                            return;
                        }
                    }
                }
            }
        }

        void release_table_writer_lock() {
            while(true) {
                uint64_t oldMetadata = value;
                uint64_t newMetadata = (value & ~writerLockHeldMask);
                if(atomic_cmpxchg(value, oldMetadata, newMetadata)){
                    return;
                }
            }
        }
    };

    uint64_t& internalPageTableFreeIndex(PageDirectoryEntry* table){
        assert(((uint64_t)table >= (uint64_t)pageStructureVirtualBase) && ((uint64_t)table < (uint64_t)pageTableGlobalMetadataBase),
               "internalPageTableFreeIndex may only be called on *internal* page tables");
        return PageDirectoryEntry::fast_global_metadata(table).value;
    }

    static_assert(sizeof(PageDirectoryEntry) == 8, "PageDirectoryEntry of wrong size");
    static_assert(sizeof(PTMetadata) == 8, "PTMetadata of wrong size");
    static_assert(sizeof(PageDirectoryEntry[512]) == 4096, "PageDirectoryEntry improperly packed");

    uint64_t toProcessBitmapBlank[divideAndRoundUp(kernel::hal::MAX_PROCESSOR_COUNT,sizeof(uint64_t))];
    size_t meaningfulBitmapPages;

    struct PageInfo{
        kernel::mm::phys_addr physicalAddress;
        kernel::mm::virt_addr virtualAddress;

        constexpr PageInfo() = default;
        constexpr PageInfo(mm::phys_addr paddr, mm::virt_addr vaddr) : physicalAddress(paddr), virtualAddress(vaddr){}
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

    PageDirectoryEntry& getInternalPDEntryForVaddr(mm::virt_addr vaddr){
        uint64_t index = (vaddr.value - (uint64_t) pageStructureVirtualBase) / mm::PageAllocator::smallPageSize;
        return *reinterpret_cast<PageDirectoryEntry*>((uint64_t)pageStructureVirtualBase
                                                           + index * sizeof(PageDirectoryEntry));
    }

    mm::virt_addr internalPageMappingToPTAddr(PageDirectoryEntry* entry){
        auto index = ((uint64_t)entry - (uint64_t)pageStructureVirtualBase) / sizeof(PageDirectoryEntry);
        return mm::virt_addr((uint64_t)pageStructureVirtualBase + index * mm::PageAllocator::smallPageSize);
    }

    void markPageTableVaddrFree(mm::virt_addr vaddr){
        freeInternalPageTableEntry(getInternalPDEntryForVaddr(vaddr));
    }

    bool addPageToReservePool(PageInfo page){
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
            incrementedWriteHead = atomic_cmpxchg(reservePoolWriteHead, prevValue, nextValue);
        } while(!incrementedWriteHead);
        ReservePoolEntry& m = reservePool[prevValue];
        //if it happens that the read head was incremented but the entry is still populated, try spinning to wait
        //for the other processor to finish copying the info from the entry
        while(m.populated){
            asm volatile ("pause");
        }
        m.pageInfo = page;
        thread_fence();
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
            incrementedReadHead = atomic_cmpxchg(reservePoolReadHead, prevValue, nextValue);
        } while(!incrementedReadHead);
        ReservePoolEntry& m = reservePool[prevValue];
        //if it happens that the write head was incremented but the entry is still populated, try spinning to wait
        //for the other processor to finish copying the info to the entry
        while(!m.populated){
            asm volatile ("pause");
        }
        page = m.pageInfo;
        thread_fence();
        m.populated = false;
        return true;
    }

    bool addPageToOverflowPool(PageInfo page){
        bool incrementedWriteHead = false;
        uint64_t prevValue;
        getInternalPDEntryForVaddr(page.virtualAddress).setPresentState(false);
        //Try to advance the write head atomically.
        do{
            prevValue = freeOverflowWriteHead;
            uint64_t nextValue = (prevValue + 1) % FREE_OVERFLOW_POOL_SIZE;
            if(nextValue == freeOverflowReadHead){
                //If the ring buffer's full, abort!!!
                return false;
            }
            incrementedWriteHead = atomic_cmpxchg(freeOverflowWriteHead, prevValue, nextValue);
        } while(!incrementedWriteHead);
        OverflowPoolEntry& m = freeOverflowPool[prevValue];

        //readyToProcess should be set to false before the read head is advanced, so we don't need to spin here and
        //this assert is valid
        assert(!m.readyToProcess, "Free overflow ring buffer state is insane!");
        m.pageInfo = page;
        memcpy(m.toProcessBitmap, toProcessBitmapBlank, sizeof(toProcessBitmapBlank));
        thread_fence();
        m.readyToProcess = true;
        if(singleProcessorMode){
            processOverflowPool();
        }
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
            if((entry.toProcessBitmap[processorInd] & mask) == 0)
                continue;
            //If we haven't processed this entry in the freeOverflowPool yet, flush the TLB entry for the corresponding page!
            kernel::amd64::invlpg(entry.pageInfo.virtualAddress.value);
            //Then flip the bit indicating we've processed this TLB flush
            atomic_and_fetch(entry.toProcessBitmap[processorInd], ~mask, MemoryOrder::ACQUIRE);
            //If all other processors have processed this page, increment the freeOverflowReadHead!
            size_t clearBitmaps = 0;
            for(size_t i = 0; i < meaningfulBitmapPages; i++){
                if(entry.toProcessBitmap[i] == 0){
                    clearBitmaps++;
                }
            }
            if(singleProcessorMode || (clearBitmaps == meaningfulBitmapPages)){
                //This makes sure that we don't increment the freeOverflowReadHead twice if two processors simultaneously
                //determine that all others have completed their TLB flushes
                auto pageAddress = entry.pageInfo.physicalAddress;
                auto virtAddress = entry.pageInfo.virtualAddress;
                //No need to process this anymore!
                thread_fence();
                entry.readyToProcess = false;
                auto didIncrementReadHead = atomic_cmpxchg(freeOverflowReadHead, index, (index + 1) % FREE_OVERFLOW_POOL_SIZE);
                //If we were the ones to increment the read head, then we can release the page back to the page allocator
                //without fear of a double-free!
                if(didIncrementReadHead) {
                    kernel::mm::PageAllocator::freeSmallPage(pageAddress);
                    markPageTableVaddrFree(virtAddress);
                }
            }
        }
    }

    bool stealFromOverflowPool(PageInfo& info){
        bool incrementedReadHead = false;
        uint64_t prevValue;
        //Try to advance the read head atomically.
        do{
            prevValue = freeOverflowReadHead;
            uint64_t nextValue = (prevValue + 1) % FREE_OVERFLOW_POOL_SIZE;
            if(prevValue == freeOverflowWriteHead){
                //If the ring buffer's empty, abort!!!
                return false;
            }
            incrementedReadHead = atomic_cmpxchg(freeOverflowReadHead, prevValue, nextValue);
        } while(!incrementedReadHead);
        OverflowPoolEntry& m = freeOverflowPool[prevValue];
        //if it happens that the write head was incremented but the entry is still populated, try spinning to wait
        //for the other processor to finish copying the info to the entry
        while(!m.readyToProcess){
            asm volatile ("pause");
        }
        info = m.pageInfo;
        thread_fence();
        m.readyToProcess = false;
        return true;
    }

    //Initializes the metadata in the page table entryBuffer to encode a linked list allowing O(1) free virtual address
    //discovery. The free metadata makes use of 2 primary "global" table variables, and one local metadata entry per free
    //entry.
    //
    //The first is a counter for the number of *present* entryBuffer. Note that this is not the same as
    //(512 - free entryBuffer) as certain entryBuffer may not be present, but may still be used to store virtual
    //address metadata for linked tables. However, the number of present entryBuffer will either be (512 - free entryBuffer)
    //or half this quantity, depending on a bit indicating the absence or presence of a supplementary virtual address
    //table.
    //
    //The second global variable is the index of the first "free" entry. This is the head of the linked list of free
    //entryBuffer in our table.
    //
    //Note that this is only to be used internally by the page table manager – in the broader kernel, the higher level
    //abstraction of VirtualMemoryZones will solve virtual address allocation using an augmented AVL tree.
    void initializeInternalPageTableFreeMetadata(PageDirectoryEntry* table){
        assert((uint64_t) table % mm::PageAllocator::smallPageSize == 0, "misaligned page table");
        for(uint64_t i = 0; i < ENTRIES_PER_TABLE; i++){
            //We will say any offset with the highest bit set is invalid - in particular this enforces that there is
            //no "next" free entry after the last one, and this is recorded by the fact that the next entry offset
            //in the last entry is 512, which has bit 9 set.
            table[i].set_local_metadata<LOCAL_VIRT_ADDR, true>(i + 1);
            table[i].set_local_metadata<LOCAL_OCCUPIED_BIT>(false);
        }
        internalPageTableFreeIndex(table) = 0;
    }

    void initializeSetupInternalPageTableFreeMetadata(PageDirectoryEntry* table){
        assert((uint64_t) table % mm::PageAllocator::smallPageSize == 0, "misaligned page table");
        uint64_t firstUnoccupied = ENTRIES_PER_TABLE;
        for(int i = ENTRIES_PER_TABLE - 1; i >= 0; i--){
            if(table[i].value == 0){
                table[i].set_local_metadata<LOCAL_VIRT_ADDR, true>(firstUnoccupied);
                table[i].set_local_metadata<LOCAL_OCCUPIED_BIT>(false);
                firstUnoccupied = (uint64_t) i;
            }
        }
        internalPageTableFreeIndex(table) = firstUnoccupied;
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
            didSwap = atomic_cmpxchg(fullMarkers[i], oldValue, newValue);
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
            didSwap = atomic_cmpxchg(partiallyOccupiedMarkers[i], oldValue, newValue);
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
        partiallyOccupiedRingBuffer[0] = 2; //the first partially occupied page on initialization is in index 2
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
                didAdvanceHead = atomic_cmpxchg(poQueueWriteHead, prevWriteHead, next);
            } while(!didAdvanceHead);
            //write the index to the queue
            partiallyOccupiedRingBuffer[prevWriteHead] = index;
            //mark the page as not full, allowing it to be marked as full again in the future
            markFullState(index, false);
            thread_fence();
            while(true){
                if(atomic_cmpxchg(poQueueWrittenLimit, prevWriteHead, static_cast<uint16_t>((prevWriteHead + 1) % ENTRIES_PER_TABLE))){
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
            index = internalPageTableFreeIndex(table);
            if(index >= ENTRIES_PER_TABLE){
                //FIXME this should be like... an ErrorOr sort of thing. This isn't technically incorrect
                return nullptr;
            }
            PageDirectoryEntry newEntry = prevEntry;
            uint64_t nextIndex = table[index].get_local_metadata<LOCAL_VIRT_ADDR, true>();
            assert(nextIndex != index, "free pointer points to self");
            assert(nextIndex == ENTRIES_PER_TABLE || !table[nextIndex].present(), "free pointer points to currently occupied entry");
            internalPageTableFreeIndex(table) = nextIndex;
            didAllocate = atomic_cmpxchg(table[0].value, prevEntry.value, newEntry.value);
        } while(!didAllocate);
        assert(!table[index].present(), "Tried to allocate a page table entry that was already present");
        assert(!table[index].get_local_metadata<LOCAL_OCCUPIED_BIT>(), "Tried to allocate a page table entry that was already occupied");
        table[index].set_local_metadata<LOCAL_OCCUPIED_BIT>(true);
        return &table[index];
    }

    void freeInternalPageTableEntry(PageDirectoryEntry& entry){
        auto tableBase = (PageDirectoryEntry*)((uint64_t)&entry & (uint64_t)~((1 << 12) - 1));
        assert(((uint64_t)tableBase - (uint64_t)pageStructureVirtualBase)/mm::PageAllocator::smallPageSize < unpopulatedHead, "Tried to free entry in nonexistent page table");
        uint64_t entryIndex = ((uint64_t)&entry - (uint64_t)tableBase) / sizeof(PageDirectoryEntry);
        assert(entry.get_local_metadata<LOCAL_OCCUPIED_BIT>(), "Tried to free a page table entry that was already freed");
        bool didFree;
        do{
            //Save the state of the prior head of the page table. We'll want to update this since it has
            //the beginning of the free list, but we want to modify it atomically using cmpxchg
            uint64_t priorHeadIndex = internalPageTableFreeIndex(tableBase);
            //Since we're freeing the entry, we zero it out, then retain the old index stored in the head as metadata
            entry = PageDirectoryEntry(0);
            entry.set_local_metadata<LOCAL_VIRT_ADDR, true>(priorHeadIndex);
            entry.set_local_metadata<LOCAL_OCCUPIED_BIT>(false);
            //Now we construct a new head whose index points to the newly freed entry
            entry.set_local_metadata<LOCAL_OCCUPIED_BIT>(false);
            didFree = atomic_cmpxchg(internalPageTableFreeIndex(tableBase), priorHeadIndex, entryIndex);
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
            if(atomic_cmpxchg(poQueueWriteHead, prevWriteHead, nextWriteHead)){
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
                thread_fence();
                while(true){
                    if(atomic_cmpxchg(poQueueWrittenLimit, prevWriteHead, nextWriteHead)){
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
                else{
                    asm volatile("pause");
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

    WITH_GLOBAL_CONSTRUCTOR(Spinlock, reserveRefillLock);

    PageInfo allocateAndMapInternalPage() {
        auto entry = allocateInternalPageTableEntry();
        auto backingPage = mm::PageAllocator::allocateSmallPage();
        entry->setAndPreserveMetadata(PageDirectoryEntry(backingPage.value | 3 | (1 << 8)));
        return PageInfo(backingPage, internalPageMappingToPTAddr(entry));
    }

    void ensureReservePoolNotEmpty(){
        if(reservePoolReadHead == reservePoolWriteHead){
            if(reserveRefillLock.try_acquire()){
                for(auto i = 0; i < RESERVE_POOL_DEFAULT_FILL; i++){
                    //TODO handle failure - could happen under extreme contention
                    addPageToReservePool(allocateAndMapInternalPage());
                }
                reserveRefillLock.release();
            }
        }
    }

    void topUpReservePool(){
        size_t reservePoolPopulatedEntries =
                (reservePoolWriteHead + RESERVE_POOL_SIZE - reservePoolReadHead) % RESERVE_POOL_SIZE;
        if(reservePoolPopulatedEntries < RESERVE_POOL_LAZY_FILL_THRESHOLD){
            if(reserveRefillLock.try_acquire()){
                for(size_t i = 0; i < RESERVE_POOL_DEFAULT_FILL - reservePoolPopulatedEntries; i++){
                    //TODO handle failure - could happen under extreme contention
                    addPageToReservePool(allocateAndMapInternalPage());
                }
                reserveRefillLock.release();
            }
        }
    }

    PageInfo allocatePage(){
        PageInfo out;
        //first try to grab a page from the reserve pool
        if(readPageFromReservePool(out)){
            return out;
        }
        //if the reserve pool is empty, try to steal a page from the overflow pool
        if(stealFromOverflowPool(out)){
            //if we were able to steal a page, we have to be sure to remap it in the internal page table.
            auto index = (out.virtualAddress.value - (uint64_t)pageStructureVirtualBase) / mm::PageAllocator::smallPageSize;
            PageDirectoryEntry* entry = (PageDirectoryEntry*)((uint64_t)pageStructureVirtualBase + index * sizeof(PageDirectoryEntry));
            entry -> setAndPreserveMetadata(PageDirectoryEntry(out.physicalAddress.value | 3 | (1 << 8)));
            return out;
        }
        //finally if all else fails, allocate a page by hand and refill the reserve pool
        out = allocateAndMapInternalPage();
        ensureReservePoolNotEmpty();
        return out;
    }

    void freePage(PageInfo pi){
        if(addPageToReservePool(pi)){
            return;
        }
        if(addPageToOverflowPool(pi)){
            return;
        }
        //FIXME trigger an IPI or something to make room
        assertUnimplemented("need to trigger an IPI");
    }

    const auto testScale = 13;
    const size_t testSize = (1 << testScale);
    const size_t sqrtTestItr = 30;
    PageInfo entries[testSize];

    void runSillyTest(){
        kernel::klog << "allocating page table entry at " << allocateInternalPageTableEntry() << "\n";

        //PageDirectoryEntry* entryBuffer[testSize];
        for(size_t a = 1; a < sqrtTestItr; a += 2) {
            for(size_t b = 1; b < sqrtTestItr; b += 2) {
                for (size_t i = 0; i < testSize; i++) {
                    entries[(i * a) % testSize] = allocatePage();
                }
                for (size_t i = 0; i < testSize; i++) {
                    freePage(entries[(i * b) % testSize]);
                }
            }
        }
        kernel::klog << "allocating page table entry at " << allocateInternalPageTableEntry() << "\n";
        kernel::klog << "poQueueReadHead is " << poQueueReadHead << "\n";
        kernel::klog << "poQueueWrittenLimit is " << poQueueWrittenLimit << "\n";
        kernel::klog << "poQueueWriteHead is " << poQueueWriteHead << "\n";
    }

    void pushFlushPlanner(mm::FlushPlanner& planner){
        auto pid = hal::getCurrentProcessorID();
        planner._ptmInternal_setPreviousPlanner(flushPlanners[pid]);
        flushPlanners[pid] = &planner;
    }

    void popFlushPlanner(){
        auto pid = hal::getCurrentProcessorID();
        assert(flushPlanners[pid] != nullptr, "Tried to pop flush planner from empty stack");
        flushPlanners[pid] = flushPlanners[pid] -> _ptmInternal_getPreviousPlanner();
    }

    /*PartialHandle makePartialPageStructure(mm::virt_addr base, size_t size){

    }*/

    void init(size_t processorCount){
        singleProcessorMode = true;
        flushPlanners = new mm::FlushPlanner* [processorCount];
        memset(flushPlanners, 0, sizeof (mm::FlushPlanner*) * processorCount);

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

        //Initialize the metadata for the internal page directory - this is done by hand
        initializePartiallyOccupiedRingBuffer();
        //Allocate the page table entryBuffer to map in our two startup page tables and map the page directory 2 MiB into our window
        auto entryInMappingPT_for_PTMap = (PageDirectoryEntry*)&internalPageTableMapping[0];
        auto entryInMappingPT_for_PTMetadataMap = (PageDirectoryEntry*)&internalPageTableMapping[1];
        auto entryInMappingPT_for_initialPT = (PageDirectoryEntry*)&internalPageTableMapping[2];
        auto entryInInitialPT_for_PDir = (PageDirectoryEntry*)&initialInternalPageTable[0];

        //Populate the entryBuffer by hand
        entryInMappingPT_for_PTMap -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(internalPageTableMapping)).value | 3 | (1 << 8)));
        entryInMappingPT_for_PTMetadataMap -> setAndPreserveMetadata(PageDirectoryEntry(amd64::early_boot_virt_to_phys(mm::virt_addr(internalTableMetadataMapping)).value | 3 | (1 << 8)));
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
        //initializeSetupInternalPageTableFreeMetadata(getPageTableForIndex(0));

        //install the page directory
        boot_page_directory_pointer_table[509] = amd64::early_boot_virt_to_phys(mm::virt_addr(bootstrapPageDir)).value | 3;
        memset(pageTableGlobalMetadataBase, 0, mm::PageAllocator::smallPageSize * 3);
        //Since we're now storing the free list head metadata in the metadata pages, we have to initialize things in
        //a rather particular order. Namely, we have to map everything in and clear out the metadata pages, and only
        //later can we actually initialize the metadata on our first page.
        //Notably we don't need to initialize the metadata for the metadata table itself, nor the internal mapping table
        //since those are handled with special logic.
        initializeSetupInternalPageTableFreeMetadata(getPageTableForIndex(2));
        //Now that the directory's mapped in where we expect it to be, initialize the metadata!
        initializeInternalPageDirectoryFreeMetadata(pageMappingDirectory);
        topUpReservePool();

        //runSillyTest();
    }

    void* temporaryHackMapMMIOPage(mm::phys_addr paddr){
        temporaryHack(3, 1, 2026, "Use a proper MMIO mapping function!");
        assert(paddr.value % mm::PageAllocator::smallPageSize == 0, "Misaligned physical page address");
        auto entry = allocateInternalPageTableEntry();
        //Map page as global, R/W permissions, and uncachable
        entry->setAndPreserveMetadata(PageDirectoryEntry(paddr.value | 3 | (1 << 8) | (1 << 4)));
        return internalPageMappingToPTAddr(entry).as_ptr<void>();
    }
}