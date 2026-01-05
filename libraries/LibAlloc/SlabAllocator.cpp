//
// Created by Spencer Martin on 8/20/25.
//
#include <liballoc/SlabAllocator.h>
#include <liballoc/PointerArithmetic.h>
#include <core/math.h>
#include <core/utility.h>
#include <assert.h>

//#define PARANOID_CHECK_FREE_IN_RANGE

namespace LibAlloc{
    constexpr size_t slabFillPercentShift = 2;
    constexpr size_t bucketPercentTableSize = (100 >> slabFillPercentShift) + 1; // 26
    constexpr size_t bucketPercentOverlap = 20; // percent overlap
    constexpr size_t fullBucketIndex = static_cast<size_t>(-1);
    constexpr size_t freeBucketIndex = static_cast<size_t>(-2);
    constexpr size_t invalidBucketIndex = static_cast<size_t>(-3);
    constexpr size_t fullBucketReintroductionOccupancyThreshold = 90;

    constexpr size_t freeBucketReleaseThreshold = 4;
    constexpr size_t freeBucketRetainLimit = 2;

    // Compute nudge for overlap in table entries
    constexpr size_t intervalNudge = divideAndRoundUp(
        bucketPercentTableSize * bucketPercentOverlap + 100 * slabAllocatorBucketCount - 1,
        100 * slabAllocatorBucketCount);

    constexpr ConstexprArray<size_t, bucketPercentTableSize> computeLowerBucketForPercentage() {
        ConstexprArray<size_t, bucketPercentTableSize> table{};
        for (size_t i = 0; i < bucketPercentTableSize; ++i) {
            // Compute "ideal" bucket for this occupancy
            const size_t correctedIndex = max(i, intervalNudge) - intervalNudge;
            const size_t bucket = correctedIndex * slabAllocatorBucketCount / bucketPercentTableSize;

            // Clamp to valid bucket range
            table[i] = min(bucket, slabAllocatorBucketCount - 1);
        }
        return table;
    }

    constexpr ConstexprArray<size_t, bucketPercentTableSize> computeUpperBucketForPercentage() {
        ConstexprArray<size_t, bucketPercentTableSize> table{};
        for (size_t i = 0; i < bucketPercentTableSize; ++i) {
            // Compute "ideal" bucket for this occupancy
            const size_t correctedIndex = min(i + intervalNudge, bucketPercentTableSize - 1);
            const size_t bucket = correctedIndex * slabAllocatorBucketCount / bucketPercentTableSize;

            // Clamp to valid bucket range
            table[i] = min(bucket, slabAllocatorBucketCount - 1);
        }
        return table;
    }


    // Tables for runtime usage
    constexpr auto occupancyToBucketLower = computeLowerBucketForPercentage();
    constexpr auto occupancyToBucketUpper = computeUpperBucketForPercentage();

    static_assert([] {
        for (size_t i = 0; i < bucketPercentTableSize; ++i) {
            if (occupancyToBucketLower[i] > occupancyToBucketUpper[i]) {
                return false;
            }
            if (occupancyToBucketLower[i] >= slabAllocatorBucketCount) {
                return false;
            }
            if (occupancyToBucketUpper[i] >= slabAllocatorBucketCount) {
                return false;
            }
        }
       return true;
    }());

#define CHECK_OCCUPANCY_PERCENTAGE_UPPER_BOUND

    constexpr size_t getBucketIndexForOccupancyInAlloc(size_t occupancyPercentage) {
        const size_t adjustedPercentage = occupancyPercentage >> slabFillPercentShift;
#ifdef CHECK_OCCUPANCY_PERCENTAGE_UPPER_BOUND
        if (condition_unlikely(adjustedPercentage >= bucketPercentTableSize)) {return slabAllocatorBucketCount - 1;}
#endif
        return occupancyToBucketUpper[adjustedPercentage];
    }

    constexpr size_t getBucketIndexForOccupancyInFree(size_t occupancyPercentage) {
        const size_t adjustedPercentage = occupancyPercentage >> slabFillPercentShift;
#ifdef CHECK_OCCUPANCY_PERCENTAGE_UPPER_BOUND
        if (condition_unlikely(adjustedPercentage >= bucketPercentTableSize)) {return slabAllocatorBucketCount - 1;}
#endif
        return occupancyToBucketLower[adjustedPercentage];
    }

    constexpr size_t getAlignValForSlotSize(size_t slotSize) {
        return max(largestPowerOf2Dividing(slotSize), 64);
    }

    Slab::Slab(size_t slot_size, void* backing_store, size_t backing_size, SlabAllocator* alloc) {
        auto backingStoreAddr = reinterpret_cast<uintptr_t>(backing_store);
        auto backingStoreEnd = backingStoreAddr + backing_size;
        const auto align = getAlignValForSlotSize(slot_size);
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
        size_t objectCount = divideAndRoundDown(8 * backing_size, 8 * slot_size + 1);
        this -> freeList = reinterpret_cast<uint8_t*>(backingStoreAddr);
        const auto freeListSize = divideAndRoundUp(objectCount, 8ul);
        backingStoreAddr += freeListSize;
        memset(this -> freeList, 0xff, freeListSize);
#endif
        backingStoreAddr = alignUp<true>(backingStoreAddr, align);
        backing_size = backingStoreEnd - backingStoreAddr;
        size_t slotCount = divideAndRoundDown(backing_size, slot_size);
        this -> slotSize = slot_size;
        assert(slot_size >= 8, "Minimum slot size is 8 bytes");
        this -> backingStorage = reinterpret_cast<void*>(backingStoreAddr);
        this -> backingSize = backing_size;
        this -> freeCount = slotCount;
        this -> nextFree = reinterpret_cast<void*>(backingStoreAddr);
        this -> initializedHorizon = reinterpret_cast<void*>(backingStoreAddr);
        this -> numSlots = slotCount;
        assert(this -> numSlots > 1, "A slab must have more than 1 slot.");
        this -> nextInBucket = nullptr;
        this -> prevInBucket = nullptr;
        this -> allocator = alloc;
        this -> bucketIndex = invalidBucketIndex;
    }
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
    void Slab::markSlotFreeState(void* ptr, bool free) {
        const auto address = reinterpret_cast<uintptr_t>(ptr);
        const auto baseAddr = reinterpret_cast<uintptr_t>(this -> backingStorage);
        const auto index = (address - baseAddr) / this -> slotSize;
        const auto bit = index % 8;
        const auto byte = index / 8;
        const auto mask = static_cast<uint8_t>(1 << bit);
        this -> freeList[byte] &= ~mask;
        this -> freeList[byte] |= (free ? mask : 0);
    }

    bool Slab::isSlotFree(void* ptr) const{
        const auto address = reinterpret_cast<uintptr_t>(ptr);
        const auto baseAddr = reinterpret_cast<uintptr_t>(this -> backingStorage);
        const auto index = (address - baseAddr) / this -> slotSize;
        const auto bit = index % 8;
        const auto byte = index / 8;
        return (this -> freeList[byte] & (1 << bit)) != 0;
    }

    bool Slab::isFree(void *ptr) const {
        return isSlotFree(ptr);
    }

#endif

    void* Slab::alloc() {
        void* toReturn = this -> nextFree;
        assert(toReturn != nullptr, "Caller did not confirm slab allocator has free space");
        //If the slot we're returning is uninitialized, then it does not contain a pointer to the next free slot
        if(condition_unlikely(toReturn == initializedHorizon)){
            auto horizonAddr = reinterpret_cast<uintptr_t>(this -> initializedHorizon);
            horizonAddr += this -> slotSize;
            this -> initializedHorizon = reinterpret_cast<void*>(horizonAddr);
            const auto backingStoreAddr = reinterpret_cast<uintptr_t>(this -> backingStorage);
            const auto backingStoreEnd = backingStoreAddr + this -> backingSize;
            //If the slot at the horizon goes past the end of the backing store, then the allocator is full
            //so we indicate this by setting nextFree to null
            if(condition_unlikely(horizonAddr + this -> slotSize > backingStoreEnd)){
                this -> nextFree = nullptr;
            }
            else{
                //Otherwise the next free slot is the next slot on the horizon
                this -> nextFree = reinterpret_cast<void*>(horizonAddr);
            }
        }
        //If the slot was already initialized, then it contains a pointer to the next free slot! We update nextFree
        //accordingly.
        else{
            void*& nextFreeSlot = *reinterpret_cast<void**>(toReturn);
            this -> nextFree = nextFreeSlot;
            nextFreeSlot = nullptr;
        }
        this -> freeCount--;
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
        markSlotFreeState(toReturn, false);
#endif
        return toReturn;
    }

    bool Slab::contains(void* ptr) const{
        const auto ptrAddr = reinterpret_cast<uintptr_t>(ptr);
        const auto backingStoreAddr = reinterpret_cast<uintptr_t>(this -> backingStorage);
        const auto backingStoreEnd = backingStoreAddr + this -> backingSize;
        return ptrAddr >= backingStoreAddr && ptrAddr < backingStoreEnd;
    }

    bool Slab::containsWithAlignment(void *ptr) const {
        if (!contains(ptr)) {
            return false;
        }
        const auto ptrAddr = reinterpret_cast<uintptr_t>(ptr);
        const auto backingStoreAddr = reinterpret_cast<uintptr_t>(this -> backingStorage);
        const auto offset = ptrAddr - backingStoreAddr;
        return offset % this -> slotSize == 0;
    }


    void Slab::free(void* ptr) {
#ifdef PARANOID_CHECK_FREE_IN_RANGE
        assert(contains(ptr), "Freeing pointer not contained in allocator");
#endif
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
        assert(!isSlotFree(ptr), "Double-freeing pointer");
        markSlotFreeState(ptr, true);
#endif
        void*& nextFreeSlot = *reinterpret_cast<void**>(ptr);
        nextFreeSlot = this -> nextFree;
        this -> nextFree = ptr;
        this -> freeCount++;
    }

    size_t Slab::getFreeCount() const{
        return this -> freeCount;
    }

    size_t Slab::getOccupancyPercent() const{
        return 100 - divideAndRoundUp(this -> freeCount * 100, this -> numSlots);
    }

    bool Slab::isFull() const{
        return this -> freeCount == 0;
    }

    bool Slab::isEmpty() const{
        return this -> freeCount == this -> numSlots;
    }

    SlabAllocator *Slab::getAllocator() const {
        return this -> allocator;
    }


    Slab* initializeSlab(void* memory, const size_t slotSize, const size_t backingSize, SlabAllocator* allocator) {
        const auto memoryAddr = reinterpret_cast<uintptr_t>(memory);
        auto bufferStart = memoryAddr + sizeof(Slab);
        return new (memory) Slab(slotSize, reinterpret_cast<void*>(bufferStart), backingSize - sizeof(Slab), allocator);
    }

    SlabAllocator::SlabAllocator(const size_t slot_size, const size_t desired_slab_size, Allocator &backing_allocator, SlabTreeType& slab_tree) :
    slotSize(slot_size), desiredSlabSize(desired_slab_size), backingAllocator(backing_allocator), slabTree(slab_tree) {
        fullSlabs = nullptr;
        freeSlabs = nullptr;
        numNonFullSlabs = 0;
        numFreeSlabs = 0;
        topOccupiedBucket = nullptr;
        for(auto & bucket : partiallyFullBuckets){
            bucket = nullptr;
        }
    }

    void SlabAllocator::releaseAllFromBucket(Slab*& bucket) {
        bool shouldUpdateNonFullBucketCount = (&bucket != &fullSlabs);
        bool shouldUpdateFreeBucketCount = (&bucket == &freeSlabs);
        while (bucket != nullptr) {
            Slab* next = bucket -> nextInBucket;
            slabTree.erase(bucket);
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
            numSlabs--;
            backingSize -= bucket -> backingSize;
#endif
            backingAllocator.free(bucket);
            bucket = next;
            if (shouldUpdateNonFullBucketCount)
                --numNonFullSlabs;
            if (shouldUpdateFreeBucketCount)
                --numFreeSlabs;
        }
        bucket = nullptr;
    }

    SlabAllocator::~SlabAllocator() {
        releaseAllFromBucket(fullSlabs);
        for(auto & bucket : partiallyFullBuckets) {
            releaseAllFromBucket(bucket);
        }
        releaseAllFromBucket(freeSlabs);
    }

    bool SlabAllocator::addNewSlab(const size_t requestedSize) {
        void* backingBuffer = backingAllocator.allocate(requestedSize, std::align_val_t{alignof(void*)});
        //Let's expect that we're not OOM
        if(condition_unlikely(backingBuffer == nullptr)) {
            return false;
        }
        auto* newSlab = initializeSlab(backingBuffer, slotSize, requestedSize, this);
        slabTree.insert(newSlab);
        newSlab -> nextInBucket = freeSlabs;
        newSlab -> bucketIndex = freeBucketIndex;
        freeSlabs = newSlab;
        ++numNonFullSlabs;
        ++numFreeSlabs;
        //We're probably calling this because we've run out of slabs to allocate from
        if (condition_likely(topOccupiedBucket == nullptr)) {
            topOccupiedBucket = &freeSlabs;
        }
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
        backingSize += requestedSize;
        numSlabs++;
#endif
        return true;
    }

    void SlabAllocator::removeSlabFromBucket(Slab& slab, Slab*& bucket) {
        if (slab.nextInBucket != nullptr) {
            slab.nextInBucket -> prevInBucket = slab.prevInBucket;
        }
        if (slab.prevInBucket != nullptr) {
            slab.prevInBucket -> nextInBucket = slab.nextInBucket;
        }
        //If slab was the head of its bucket, then we reassign the bucket head to the next slab
        else {
            bucket = slab.nextInBucket;
        }
        slab.nextInBucket = nullptr;
        slab.prevInBucket = nullptr;
    }

    void SlabAllocator::insertSlabAtBucketHead(Slab &slab, Slab *&bucket) {
        slab.nextInBucket = bucket;
        if (bucket != nullptr) {
            bucket -> prevInBucket = &slab;
        }
        bucket = &slab;
    }

    void* SlabAllocator::alloc() {
        //Usually we won't be allocating a new slab.
        if (condition_unlikely(numNonFullSlabs == 0)) {
            addNewSlab(desiredSlabSize);
        }

        auto* targetSlab = *topOccupiedBucket;
        auto* toReturn = targetSlab -> alloc();
        if (condition_unlikely(targetSlab -> isFull())) {
            removeSlabFromBucket(*targetSlab, *topOccupiedBucket);
            insertSlabAtBucketHead(*targetSlab, fullSlabs);
            targetSlab -> bucketIndex = fullBucketIndex;
            --numNonFullSlabs;
            if (numNonFullSlabs == 0) {
                topOccupiedBucket = nullptr;
            }
            else {
                topOccupiedBucket = &freeSlabs;
                for (int i = slabAllocatorBucketCount - 1; i >= 0; --i) {
                    if (partiallyFullBuckets[i] != nullptr) {
                        topOccupiedBucket = &partiallyFullBuckets[i];
                        break;
                    }
                }
            }
        }
        else if (condition_unlikely(topOccupiedBucket == &freeSlabs
            || targetSlab -> bucketIndex != getBucketIndexForOccupancyInAlloc(targetSlab -> getOccupancyPercent()))) {
            if (topOccupiedBucket == &freeSlabs) numFreeSlabs--;
            const auto newIndex = getBucketIndexForOccupancyInAlloc(targetSlab -> getOccupancyPercent());
            removeSlabFromBucket(*targetSlab, *topOccupiedBucket);

            targetSlab -> bucketIndex = newIndex;
            insertSlabAtBucketHead(*targetSlab, partiallyFullBuckets[newIndex]);

            //Since we were already allocating from the highest occupied bucket, and the index of targetSlab just increased,
            //we know that the new top-occupied bucket is wherever targetSlab ended up.
            topOccupiedBucket = &partiallyFullBuckets[newIndex];
        }
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
        currentlyAllocatedSize += slotSize;
        netAllocatedSize += slotSize;
#endif
        return toReturn;
    }

    inline void SlabAllocator::releaseFreeSlabsIfNecessary() {
        if (condition_unlikely(numFreeSlabs > freeBucketReleaseThreshold)) {
            while (numFreeSlabs > freeBucketRetainLimit) {
                auto* slab = freeSlabs;
                freeSlabs = slab -> nextInBucket;
                slabTree.erase(slab);
                freeSlabs -> prevInBucket = nullptr;
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
                --numSlabs;
                backingSize -= slab ->  backingSize;
#endif
                backingAllocator.free(slab);
                --numFreeSlabs;
                --numNonFullSlabs;
            }
        }
        assert(freeSlabs != nullptr || topOccupiedBucket != &freeSlabs, "topOccupiedBucket should never point to null");
    }

    void SlabAllocator::free(void *ptr, Slab& parentSlab) {
        //We will assume that the caller did their due-diligence and found the correct slab, since this
        //is meant to be used as a component in a larger, more complete allocator
        parentSlab.free(ptr);
        //To try to prevent thrashing with the full bucket and the top partially occupied bucket, we'll delay
        //marking a slab as no longer full until its occupancy falls below a given threshold. I am not concerned
        //about the reverse phenomenon with the free slab bucket, since we would only risk thrashing if all other
        //slabs were either marked as full or completely empty.
        if (condition_unlikely(parentSlab.bucketIndex == fullBucketIndex)) {
            if (condition_unlikely(parentSlab.getOccupancyPercent() <= fullBucketReintroductionOccupancyThreshold)) {
                removeSlabFromBucket(parentSlab, fullSlabs);
                size_t newIndex = slabAllocatorBucketCount - 1;
                ++numNonFullSlabs;
                insertSlabAtBucketHead(parentSlab, partiallyFullBuckets[newIndex]);
                topOccupiedBucket = &partiallyFullBuckets[newIndex];
                parentSlab.bucketIndex = newIndex;
            }
        }
        else if (condition_unlikely(parentSlab.isEmpty())) {
            removeSlabFromBucket(parentSlab, partiallyFullBuckets[parentSlab.bucketIndex]);
            insertSlabAtBucketHead(parentSlab, freeSlabs);
            ++numFreeSlabs;
            parentSlab.bucketIndex = freeBucketIndex;
            //If parentSlab was not full, then there's no way that topOccupiedBucket is null. Thus, it's safe to
            //dereference it and check if the bucket it points to is empty. If so, then we know that
            //parentSlab was the only partially occupied bucket, so we change topOccupiedBucket to point to the
            //free bucket
            if (*topOccupiedBucket == nullptr) {
                topOccupiedBucket = &freeSlabs;
            }
            releaseFreeSlabsIfNecessary();
        }
        else if (condition_unlikely(parentSlab.bucketIndex != getBucketIndexForOccupancyInFree(parentSlab.getOccupancyPercent()))) {
            const size_t oldIndex = parentSlab.bucketIndex;
            removeSlabFromBucket(parentSlab, partiallyFullBuckets[parentSlab.bucketIndex]);
            const size_t newIndex = getBucketIndexForOccupancyInFree(parentSlab.getOccupancyPercent());
            parentSlab.bucketIndex = newIndex;
            insertSlabAtBucketHead(parentSlab, partiallyFullBuckets[parentSlab.bucketIndex]);
            //If parentSlab was not full, then there's no way that topOccupiedBucket is null. Thus, it's safe to
            //dereference it and check if the bucket it points to is empty. If so, then we know that parentSlab
            //was the most occupied non-full slab, and its bucket got drained. Thus, the most occupied bucket is now
            //the bucket that parentSlab belongs to
            if (*topOccupiedBucket == nullptr) {
                for (int i = (int)oldIndex; i >= (int)parentSlab.bucketIndex; --i) {
                    if (partiallyFullBuckets[i] != nullptr) {
                        topOccupiedBucket = &partiallyFullBuckets[i];
                        break;
                    }
                }
            }
        }
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
        currentlyAllocatedSize -= slotSize;
        netFreedSize += slotSize;
#endif
    }

    SlabAllocatorStats SlabAllocator::getStatistics() const {
        SlabAllocatorStats stats{};
        stats.numSlabs = numSlabs;
        stats.numFreeSlabs = numFreeSlabs;
        stats.numNonFullSlabs = numNonFullSlabs;
        stats.currentlyAllocatedSize = currentlyAllocatedSize;
        stats.netAllocatedSize = netAllocatedSize;
        stats.netFreedSize = netFreedSize;
        stats.totalBackingSize = backingSize;
        stats.totalMetadataSize = 0; //TODO
        return stats;
    }

    void SlabAllocator::releaseAllFreeSlabs() {
        releaseAllFromBucket(freeSlabs);
    }

}