//
// Created by Spencer Martin on 8/20/25.
//

#ifndef SLABALLOCATOR_H
#define SLABALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <core/utility.h>
#include <core/ds/Trees.h>
#include <liballoc/Allocator.h>
#include <core/TypeTraits.h>
#include <core/debug/DbgStddef.h>

#define SLAB_ALLOCATOR_KEEP_FREE_LIST

#define SLAB_ALLOCATOR_KEEP_STATISTICS

namespace LibAlloc {
    constexpr size_t slabAllocatorBucketCount = 6;

    class SlabAllocator;
    class Slab {
        friend class SlabAllocator;
        SlabAllocator *allocator;
        size_t slotSize;
        void *backingStorage;
        void *initializedHorizon;
        size_t backingSize;
        size_t freeCount;
        size_t numSlots;
        void *nextFree;
        Slab* nextInBucket;
        Slab* prevInBucket;
        size_t bucketIndex;

        //Metadata for tracking slabs by address to use in freeing
        Slab* leftChild;
        Slab* rightChild;
        Slab* parent;
        bool color;
        friend struct SlabNodeInfoExtractor;
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
        uint8_t *freeList;

        void markSlotFreeState(void *ptr, bool free);

        bool isSlotFree(void *ptr) const;
#endif

    public:
        Slab(size_t slotSize, void *backingStorage, size_t backingSize, SlabAllocator* allocator);

        bool contains(void *ptr) const;
        bool containsWithAlignment(void* ptr) const;
#ifdef SLAB_ALLOCATOR_KEEP_FREE_LIST
        bool isFree(void *ptr) const;
#endif
        void free(void *ptr);
        void *alloc();
        size_t getFreeCount() const;
        size_t getOccupancyPercent() const;
        bool isFull() const;
        bool isEmpty() const;

        SlabAllocator* getAllocator() const;
    };

    struct SlabNodeInfoExtractor {
        static Slab*& left(Slab& slab){return slab.leftChild;}
        static Slab* const& left(const Slab& slab) {return slab.leftChild;}
        static Slab*& right(Slab& slab){return slab.rightChild;}
        static Slab* const& right(const Slab& slab) {return slab.rightChild;}
        static Slab*& parent(Slab& slab){return slab.parent;}
        static Slab* const& parent(const Slab& slab) {return slab.parent;}
        static bool isRed(Slab& slab){return slab.color;}
        static void setRed(Slab& slab, bool red){slab.color = red;}
        static uintptr_t data(Slab& slab) {return reinterpret_cast<uintptr_t>(&slab);}
        static uintptr_t data(const Slab& slab) {return reinterpret_cast<uintptr_t>(&slab);}
    };

    using SlabTreeType = IntrusiveRedBlackTree<Slab, SlabNodeInfoExtractor>;

    template <typename T>
    concept BackingAllocator = requires(T t, size_t size)
    {
        {t.alloc(size)} -> convertible_to<void*>;
        t.free(nullptr);
    };

    struct SlabAllocatorStats {
        size_t totalBackingSize;
        size_t totalMetadataSize;
        size_t currentlyAllocatedSize;
        size_t netAllocatedSize;
        size_t netFreedSize;
        size_t numSlabs;
        size_t numFreeSlabs;
        size_t numNonFullSlabs;
    };



    class SlabAllocator {
        const size_t slotSize;
        const size_t desiredSlabSize;
        Slab* fullSlabs;
        Slab* partiallyFullBuckets[slabAllocatorBucketCount]{};
        volatile Slab* dummy;
        Slab* freeSlabs;
        size_t numFreeSlabs;
        size_t numNonFullSlabs;
        Slab** topOccupiedBucket;
        Allocator& backingAllocator;
        SlabTreeType& slabTree;
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
        size_t backingSize;
        size_t currentlyAllocatedSize;
        size_t netAllocatedSize;
        size_t netFreedSize;
        size_t numSlabs;
#endif

        void releaseAllFromBucket(Slab*& bucket);
        bool addNewSlab(size_t requestedSize);
        static void removeSlabFromBucket(Slab& slab, Slab*& bucket);
        static void insertSlabAtBucketHead(Slab& slab, Slab*& bucket);
        inline void releaseFreeSlabsIfNecessary();
    public:
        SlabAllocator(size_t slot_size, size_t desired_slab_size, Allocator& backing_allocator, SlabTreeType& slab_tree);
        ~SlabAllocator();

        [[nodiscard]] void* alloc();
        //We expect the caller to have already identified the slab which owns ptr
        void free(void *ptr, Slab& parentSlab);
        void releaseAllFreeSlabs();
#ifdef SLAB_ALLOCATOR_KEEP_STATISTICS
        [[nodiscard]] SlabAllocatorStats getStatistics() const;
#endif
    };
}
#endif //SLABALLOCATOR_H
