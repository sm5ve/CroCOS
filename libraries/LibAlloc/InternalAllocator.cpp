//
// Created by Spencer Martin on 8/1/25.
//

#include <stddef.h>
#include <liballoc/Backend.h>
#include <liballoc/PointerArithmetic.h>
#include <core/ds/Trees.h>
#include <stdint.h>
#include <liballoc/InternalAllocator.h>
#include <liballoc/InternalAllocatorDebug.h>
#include <core/SizeClass.h>
#include <liballoc/Allocator.h>
#include <liballoc/SlabAllocator.h>

#define ASSUME_ALIGN_POWER_OF_TWO

namespace LibAlloc::InternalAllocator {

#ifdef ASSUME_ALIGN_POWER_OF_TWO
    constexpr bool AssumePowerOfTwoAlignment = true;
#else
    constexpr bool AssumePowerOfTwoAlignment = false;
#endif

    constexpr ConstexprArray slabSizeClasses = {8ul, 16, 32, 64, 96, 128, 256, 512, };
    constexpr ConstexprArray slabAllocatorBufferSizes = {1024ul, 1024, 1024, 2048, 2048, 2048, 2048, 8192, };
    static_assert(slabSizeClasses.size() == slabAllocatorBufferSizes.size(), "Size classes and buffer sizes must be the same size");
    constexpr auto sizeClassJumpTable = makeSizeClassJumpTable<slabSizeClasses>();

    struct alignas(alignof(max_align_t)) UnallocatedMemoryBlockHeader {
        size_t sizeAndColor; //includes the size of the header itself
        size_t maxSizeBlockInSubtree;
        UnallocatedMemoryBlockHeader* left;
        UnallocatedMemoryBlockHeader* right;
        UnallocatedMemoryBlockHeader* next;
        UnallocatedMemoryBlockHeader* prev;


        [[nodiscard]] size_t size() const {return sizeAndColor & ~3u;}
        [[nodiscard]] bool isRed() const {return ((sizeAndColor & 1u) == 1);}
        [[nodiscard]] bool isAddressRed() const {return ((sizeAndColor & 2u) == 2);}
        void setRed(bool red) {sizeAndColor &= ~1u; sizeAndColor |= red ? 1 : 0;}
        void setAddressRed(bool red) {sizeAndColor &= ~2u; sizeAndColor |= red ? 2 : 0;}

        bool operator==(const UnallocatedMemoryBlockHeader& other) const {
            return this == &other;
        }
    };

    struct UnallocatedMemoryBlockInfoExtractor {
        static UnallocatedMemoryBlockHeader*& left(UnallocatedMemoryBlockHeader& header){return header.left;}
        static UnallocatedMemoryBlockHeader*& right(UnallocatedMemoryBlockHeader& header){return header.right;}
        static UnallocatedMemoryBlockHeader& data(UnallocatedMemoryBlockHeader& header) {return header;}
        static UnallocatedMemoryBlockHeader* const& left(const UnallocatedMemoryBlockHeader& header){return header.left;}
        static UnallocatedMemoryBlockHeader* const& right(const UnallocatedMemoryBlockHeader& header){return header.right;}
        static const UnallocatedMemoryBlockHeader& data(const UnallocatedMemoryBlockHeader& header) {return header;}
        static bool isRed(UnallocatedMemoryBlockHeader& header){return header.isRed();}
        static void setRed(UnallocatedMemoryBlockHeader& header, bool red){header.setRed(red);}
        static size_t& augmentedData(UnallocatedMemoryBlockHeader& header){return header.maxSizeBlockInSubtree;}
        static size_t recomputeAugmentedData(const UnallocatedMemoryBlockHeader& header, const UnallocatedMemoryBlockHeader* left, const UnallocatedMemoryBlockHeader* right) {
            const auto size = header.size();
            const auto leftSize = left ? left -> maxSizeBlockInSubtree : 0;
            const auto rightSize = right ? right -> maxSizeBlockInSubtree : 0;
            return max(size, leftSize, rightSize);
        }
    };

    struct UnallocatedMemoryBlockAddressInfoExtractor {
        static UnallocatedMemoryBlockHeader*& left(UnallocatedMemoryBlockHeader& header){return header.prev;}
        static UnallocatedMemoryBlockHeader*& right(UnallocatedMemoryBlockHeader& header){return header.next;}
        static UnallocatedMemoryBlockHeader* const& left(const UnallocatedMemoryBlockHeader& header){return header.prev;}
        static UnallocatedMemoryBlockHeader* const& right(const UnallocatedMemoryBlockHeader& header){return header.next;}
        static uintptr_t data(const UnallocatedMemoryBlockHeader& header) {return reinterpret_cast<uintptr_t>(&header);}
        static bool isRed(UnallocatedMemoryBlockHeader& header){return header.isAddressRed();}
        static void setRed(UnallocatedMemoryBlockHeader& header, bool red){header.setAddressRed(red);}
    };

    struct UnallocatedMemoryBlockComparator {
        bool operator()(const UnallocatedMemoryBlockHeader& a, const UnallocatedMemoryBlockHeader& b) const {
            if (a.size() == b.size()) {
                return reinterpret_cast<uintptr_t>(&a) < reinterpret_cast<uintptr_t>(&b);
            }
            return a.size() < b.size();
        }
    };

    //We can eliminate the need for AlignedAllocatedMemoryBlockHeader with some clever packing in
    //sizeAndColor if ASSUME_ALIGN_POWER_OF_TWO is set. This may not save any space or be remotely advantageous, though.
    struct alignas(alignof(max_align_t)) AllocatedMemoryBlockHeader {
        size_t sizeAndColor; //includes the size of the header itself
        AllocatedMemoryBlockHeader* left;
        AllocatedMemoryBlockHeader* right;
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        size_t requestedSize;
#endif

        [[nodiscard]] uint32_t size() const {return sizeAndColor & ~3u;}
        [[nodiscard]] bool isRed() const {return ((sizeAndColor & 1u) == 1);}
        [[nodiscard]] bool isAligned() const {return ((sizeAndColor & 2u) == 2);}
        void setRed(bool red) {sizeAndColor &= ~1u; sizeAndColor |= red ? 1 : 0;}
        void setAligned(bool aligned) {sizeAndColor &= ~2u; sizeAndColor |= aligned ? 2 : 0;}

        bool operator==(const AllocatedMemoryBlockHeader& other) const {
            return this == &other;
        }
    };

    struct alignas(alignof(max_align_t)) AlignedAllocatedMemoryBlockHeader : AllocatedMemoryBlockHeader {
        void* dataBegin;
    };

    struct AllocatedMemoryBlockInfoExtractor {
        static AllocatedMemoryBlockHeader*& left(AllocatedMemoryBlockHeader& header){return header.left;}
        static AllocatedMemoryBlockHeader*& right(AllocatedMemoryBlockHeader& header){return header.right;}
        static AllocatedMemoryBlockHeader* const& left(const AllocatedMemoryBlockHeader& header){return header.left;}
        static AllocatedMemoryBlockHeader* const& right(const AllocatedMemoryBlockHeader& header){return header.right;}
        static uintptr_t data(const AllocatedMemoryBlockHeader& header) {return reinterpret_cast<uintptr_t>(&header);}
        static bool isRed(AllocatedMemoryBlockHeader& header){return header.isRed();}
        static void setRed(AllocatedMemoryBlockHeader& header, bool red){header.setRed(red);}
    };

    //Smallest block size we will allocate is at least twice the size of the largest block header
    constexpr size_t minimumBlockSize = 2 * max(sizeof(UnallocatedMemoryBlockHeader),
        sizeof(AllocatedMemoryBlockHeader), sizeof(AlignedAllocatedMemoryBlockHeader));

#ifdef TRACK_REQUESTED_ALLOCATION_STATS
#define SPAN_ALLOC_STAT_LIST size_t& requestedAllocationStat, size_t& committedAllocationStat
#else
#define SPAN_ALLOC_STAT_LIST size_t& committedAllocationStat
#endif

    //Each span belongs to 2 red-black trees - one ordered by starting address, and one by remaining free space
    //The latter RBT is augmented to keep track of the largest free block in the given span
    struct alignas(alignof(max_align_t)) MemorySpanHeader {
        size_t spanSize; //in bytes, including header

        struct { //Store red/black metadata for
            uint8_t unallocatedTreeColor : 1;
            uint8_t allocatedTreeColor : 1;
            uint8_t releasable : 1;
        } flags{};

        MemorySpanHeader* unallocatedTreeLeftChild;
        MemorySpanHeader* unallocatedTreeRightChild;
        //Since the malloc tree is ordered by free space, which is quite volatile, we include a parent pointer to
        //allow for optimized in-place updates when possible
        MemorySpanHeader* unallocatedTreeParent;

        MemorySpanHeader* allocatedTreeLeftChild;
        MemorySpanHeader* allocatedTreeRightChild;
        MemorySpanHeader* allocatedTreeParent;

        IntrusiveRedBlackTree<AllocatedMemoryBlockHeader, AllocatedMemoryBlockInfoExtractor>
            allocatedBlockTree;
        IntrusiveRedBlackTree<UnallocatedMemoryBlockHeader, UnallocatedMemoryBlockInfoExtractor, UnallocatedMemoryBlockComparator>
            unallocatedBlockTree;
        IntrusiveRedBlackTree<UnallocatedMemoryBlockHeader, UnallocatedMemoryBlockAddressInfoExtractor>
            unallocatedBlocksByAddress;

        size_t freeSpace; //In bytes, including headers for each block
        size_t largestFreeBlockSize; //In bytes, including headers for each block
        size_t largestFreeBlockInMallocSubtree; //In bytes, including headers for each block

        private:
        void insertFreeBlock(UnallocatedMemoryBlockHeader* block);
        void removeFreeBlock(UnallocatedMemoryBlockHeader* block);
        void coalesceAdjacentFreeBlocks(UnallocatedMemoryBlockHeader* block);
        [[nodiscard]] AllocatedMemoryBlockHeader* getValidatedHeaderForPtr(void* ptr) const;
        public:
        explicit MemorySpanHeader(size_t size);
        void markUnreleasable();
        void* allocateBlock(size_t size, std::align_val_t align, SPAN_ALLOC_STAT_LIST);
        bool freeBlock(void* ptr, SPAN_ALLOC_STAT_LIST);
        [[nodiscard]]size_t getBufferSize() const;
        [[nodiscard]] bool isPointerAllocated(void* ptr) const;

        bool operator==(const MemorySpanHeader& other) const {
            return this == &other;
        }
    };

    size_t MemorySpanHeader::getBufferSize() const {
        auto* header = offsetPointerByBytesAndAlign<UnallocatedMemoryBlockHeader>(this, sizeof(MemorySpanHeader));
        return reinterpret_cast<uintptr_t>(this) + this -> spanSize - reinterpret_cast<uintptr_t>(header);
    }

    MemorySpanHeader::MemorySpanHeader(const size_t size) {
        flags.releasable = true;
        spanSize = size;
        auto* header = offsetPointerByBytesAndAlign<UnallocatedMemoryBlockHeader>(this, sizeof(MemorySpanHeader));
        freeSpace = getBufferSize();
        header -> sizeAndColor = freeSpace;
        unallocatedTreeLeftChild = nullptr;
        unallocatedTreeRightChild = nullptr;
        unallocatedTreeParent = nullptr;
        allocatedTreeLeftChild = nullptr;
        allocatedTreeRightChild = nullptr;
        allocatedTreeParent = nullptr;
        largestFreeBlockSize = freeSpace;
        largestFreeBlockInMallocSubtree = freeSpace;
        flags.unallocatedTreeColor = false;
        flags.allocatedTreeColor = false;
        insertFreeBlock(header);
    }

    void MemorySpanHeader::markUnreleasable() {
        flags.releasable = false;
    }


    void MemorySpanHeader::insertFreeBlock(UnallocatedMemoryBlockHeader *block) {
        size_t freeSize = block -> size();
        unallocatedBlockTree.template insert<StaticStack<UnallocatedMemoryBlockHeader**, 64>>(block);
        unallocatedBlocksByAddress.template insert<StaticStack<UnallocatedMemoryBlockHeader**, 64>>(block);
        //largestFreeBlockSize is used to populate the augmentation data for the span tree (and not the memory block trees).
        //We only ever call this method from within a .update() context for the span tree, so the augmentation data will
        //be automatically updated
        if (freeSize > largestFreeBlockSize) {
            largestFreeBlockSize = freeSize;
        }
    }

    void MemorySpanHeader::removeFreeBlock(UnallocatedMemoryBlockHeader *block) {
        unallocatedBlocksByAddress.template erase<StaticStack<UnallocatedMemoryBlockHeader**, 64>>(block);
        unallocatedBlockTree.template erase<StaticStack<UnallocatedMemoryBlockHeader**, 64>>(block);
        //largestFreeBlockSize is used to populate the augmentation data for the span tree (and not the memory block trees).
        //We only ever call this method from within a .update() context for the span tree, so the augmentation data will
        //be automatically updated
        if (this -> largestFreeBlockSize == block -> size()) {
            auto maxFreeBlock = unallocatedBlockTree.max();
            if (maxFreeBlock != nullptr) {
                this -> largestFreeBlockSize = maxFreeBlock -> size();
            }
            else {
                this -> largestFreeBlockSize = 0;
            }
        }
    }

    //Memory block headers are automatically aligned to some degree.
    //This computes the guaranteed alignment of the memory immediately after an AllocatedMemoryBlockHeader.
    //If the requested alignment of the allocation is at most guaranteedAlignAfterBlock, then we can put the allocation
    //Immediately after the header. Otherwise, we'll need to use the slightly larger AlignedAllocatedMemoryBlockHeader
    //To account for the offset.
    constexpr size_t guaranteedAlignAfterBlock = min(largestPowerOf2Dividing(sizeof(AllocatedMemoryBlockHeader)), alignof(AllocatedMemoryBlockHeader));

    //Computes the worst case total necessary size for a block to fulfill an allocation request.
    //If the requested alignment is sufficiently low, this is just size + sizeof(AllocatedMemoryBlockHeader).
    //Otherwise, we will need to use the larger AlignedAllocatedMemoryBlockHeader and require some extra wiggle room to
    //we can guarantee appropriate alignment

    constexpr size_t computeWorstCaseAlignedSize(const size_t size, const std::align_val_t align) {
        auto alignSize = static_cast<size_t>(align);
        size_t paddedSize = size;
#ifdef ASSUME_ALIGN_POWER_OF_TWO
        if (guaranteedAlignAfterBlock & (alignSize - 1))
#else
        if (alignSize % guaranteedAlignAfterBlock != 0)
#endif
        {
            paddedSize += alignSize;
            paddedSize += sizeof(AlignedAllocatedMemoryBlockHeader);
        }
        else {
            paddedSize += sizeof(AllocatedMemoryBlockHeader);
        }
        return paddedSize;
    }

    constexpr size_t alignedHeaderMetadataSize = sizeof(AlignedAllocatedMemoryBlockHeader) - sizeof(AllocatedMemoryBlockHeader);

    //Given the start address for a block of free memory, it finds the first aligned address suitable to fulfill the
    //allocation request, taking into account the space needed for the appropriate header before.
    constexpr uintptr_t findFirstAlignedAddressAfterHeaderSpace(uintptr_t addr, std::align_val_t align) {
        auto alignSize = static_cast<size_t>(align);
        uintptr_t outAddr = addr + sizeof(AllocatedMemoryBlockHeader);
#ifdef ASSUME_ALIGN_POWER_OF_TWO
        if (outAddr & (alignSize - 1))
#else
        if (outAddr % alignSize != 0)
#endif
        {
            outAddr += alignedHeaderMetadataSize;
            outAddr = alignUp<AssumePowerOfTwoAlignment>(outAddr, alignSize);
        }
        return outAddr;
    }

    //In cases of extreme alignment, it is possible that the reserved memory will start well into a given free block.
    //In this case, we may hope to keep this lower chunk of memory free, and put the allocated memory block header closer
    //to the return address.
    constexpr uintptr_t findFirstAlignedHeaderLocationBelowAddr(uintptr_t addr) {
        return alignDown<AssumePowerOfTwoAlignment>(addr - sizeof(AllocatedMemoryBlockHeader), alignof(AllocatedMemoryBlockHeader));
    }

    void* MemorySpanHeader::allocateBlock(size_t size, std::align_val_t align, SPAN_ALLOC_STAT_LIST) {
        size_t paddedSize = computeWorstCaseAlignedSize(size, align);
        //This is a little conservative - there are edge cases where we might be able to fulfill a request with a slightly
        //smaller free block if it is conveniently aligned. But checking for that is rather inefficient, so we prefer
        //to bail and have the memory allocator create a new span.
        if (this -> largestFreeBlockSize < paddedSize) return nullptr;

        //Find a block that will fit our allocation
        UnallocatedMemoryBlockHeader* suitableFreeBlock = unallocatedBlockTree.mappedCeil(paddedSize, [](UnallocatedMemoryBlockHeader& header) {
            return header.size();
        });
        assert(suitableFreeBlock != nullptr, "largestFreeBlockSize seems to have been stale - suitableFreeBlock is null");
        //Retain the size of the block before removing it. In principle, I think the order does not matter, but it seems
        //best to treat the UnallocatedMemoryBlockHeader as invalid after removing the free block.
        size_t freeSize = suitableFreeBlock -> size();
        removeFreeBlock(suitableFreeBlock);

        //Necessarily aligned to alignof(AllocatedMemoryBlockHeader) == alignof(UnallocatedMemoryBlockHeader)
        uintptr_t blockBaseAddr = reinterpret_cast<uintptr_t>(suitableFreeBlock);
        uintptr_t blockEndAddr = blockBaseAddr + freeSize;
        //Finds the first suitably aligned address after accounting for header space. This will be the address of the buffer
        //that we return.
        uintptr_t returnAddr = findFirstAlignedAddressAfterHeaderSpace(blockBaseAddr, align);
        //Now we need to work backwards and find a properly aligned address to put our header
        uintptr_t headerBaseAddr = findFirstAlignedHeaderLocationBelowAddr(returnAddr);

        //If the leftover space below is sufficiently small, extend the AllocatedBlock downwards
        size_t leftoverSizeBelow = headerBaseAddr - blockBaseAddr;
        if (leftoverSizeBelow < minimumBlockSize) {
            headerBaseAddr = blockBaseAddr;
        }

        //Similarly, we need to see where the next header would go if there is enough space.
        //This should be at most blockEndAddr.
        uintptr_t nextHeaderBaseAddr = max(returnAddr + size, headerBaseAddr + minimumBlockSize);
        nextHeaderBaseAddr = alignUp<AssumePowerOfTwoAlignment>(nextHeaderBaseAddr, alignof(AllocatedMemoryBlockHeader));

        //If the leftover space above is sufficiently small, extend the AllocatedBlock upwards to avoid a small block.
        size_t leftoverSizeAbove = blockEndAddr - nextHeaderBaseAddr;
        if (leftoverSizeAbove < minimumBlockSize) {
            nextHeaderBaseAddr = blockEndAddr;
        }

        //Now we just need to create an AllocatedMemoryBlock for this allocation
        //and potentially one or two UnallocatedMemoryBlocks for any leftovers above or below our allocation.

        //We begin with the AllocatedMemoryBlock
        const auto allocatedBlock = reinterpret_cast<AllocatedMemoryBlockHeader*>(headerBaseAddr);
        allocatedBlock -> sizeAndColor = nextHeaderBaseAddr - headerBaseAddr;
        allocatedBlock -> left = nullptr;
        allocatedBlock -> right = nullptr;
        //If the return address is not immediately after the AllocatedBlockHeader, that means we need to use the
        //slightly larger AlignedAllocatedMemoryBlockHeader, which stores a pointer to the beginning of the actual
        //allocated memory. This is used to reject freeing internal pointers
        if (headerBaseAddr + sizeof(AllocatedMemoryBlockHeader) != returnAddr) {
            const auto alignedBlock = reinterpret_cast<AlignedAllocatedMemoryBlockHeader*>(headerBaseAddr);
            alignedBlock -> dataBegin = reinterpret_cast<void*>(returnAddr);
            alignedBlock -> setAligned(true);
        }
        else {
            allocatedBlock -> setAligned(false);
        }
        allocatedBlockTree.template insert<StaticStack<AllocatedMemoryBlockHeader**, 64>>(allocatedBlock);

        //If there's leftover space below the allocation, add it back to the free trees
        if (headerBaseAddr != blockBaseAddr) {
            const auto belowBlock = reinterpret_cast<UnallocatedMemoryBlockHeader*>(blockBaseAddr);
            belowBlock -> sizeAndColor = headerBaseAddr - blockBaseAddr;
            insertFreeBlock(belowBlock);
        }

        //If there's leftover space above, also add it to the free trees
        if (nextHeaderBaseAddr != blockEndAddr) {
            const auto aboveBlock = reinterpret_cast<UnallocatedMemoryBlockHeader*>(nextHeaderBaseAddr);
            aboveBlock -> sizeAndColor = blockEndAddr - nextHeaderBaseAddr;
            insertFreeBlock(aboveBlock);
        }

        this -> freeSpace -= allocatedBlock -> size();
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        requestedAllocationStat += paddedSize;
        allocatedBlock -> requestedSize = size;
#endif
        committedAllocationStat += allocatedBlock -> size();

        return reinterpret_cast<void*>(returnAddr);
    }

    void MemorySpanHeader::coalesceAdjacentFreeBlocks(UnallocatedMemoryBlockHeader* block) {
        const auto blockAddr = reinterpret_cast<uintptr_t>(block);
        //First, we get pointers to the allocated memory block headers before and after block
        UnallocatedMemoryBlockHeader* before = unallocatedBlocksByAddress.predecessor(block);
        if (before != nullptr) {
            //If the predecessor is not immediately before the block, reject it by setting before to nullptr
            const auto beforeAddr = reinterpret_cast<uintptr_t>(before);
            if (beforeAddr + before -> size() != blockAddr) {before = nullptr;}
        }
        UnallocatedMemoryBlockHeader* after = unallocatedBlocksByAddress.successor(block);
        //If the successor is not immediately after the block, reject it by setting after to nullptr
        if (after != nullptr && blockAddr + block -> size() != reinterpret_cast<uintptr_t>(after)) {after = nullptr;}
        //If there is no allocated memory block header immediately before or after the block, then we are done.
        //There are no blocks to coalesce.
        if (before == nullptr && after == nullptr) return;

        //Otherwise, there are blocks to coalesce! We know we will have to remove the block from the free tree.
        removeFreeBlock(block);
        //And remove any adjacent blocks.
        if (before != nullptr) removeFreeBlock(before);
        if (after != nullptr) removeFreeBlock(after);

        //If there was a block immediately before this block, we combine their sizes and then set the base block pointer
        //to the predecessor.
        if (before != nullptr) {
            before -> sizeAndColor += block -> size();
            block = before;
        }
        //If there was a block immediately after, we combine their sizes, but no update to the block pointer is necessary.
        if (after != nullptr) {
            block -> sizeAndColor += after -> size();
        }
        //Finally, reinsert the coalesced free block into the free trees.
        insertFreeBlock(block);
    }

    AllocatedMemoryBlockHeader *MemorySpanHeader::getValidatedHeaderForPtr(void *ptr) const {
        auto ptrAddr = reinterpret_cast<uintptr_t>(ptr);
        AllocatedMemoryBlockHeader* blockHeader = allocatedBlockTree.floor(ptrAddr);
        //If no header exists, this was an invalid free
        if (blockHeader == nullptr) return nullptr;
        //If the block header is aligned, then it contains a pointer to the beginning of its allocated memory/
        //That pointer must match ptr, otherwise we might be trying to free an internal pointer.
        if (blockHeader -> isAligned()) {
            auto alignedBlockHeader = reinterpret_cast<AlignedAllocatedMemoryBlockHeader*>(blockHeader);
            if (alignedBlockHeader -> dataBegin != ptr) return nullptr;
        }
        //Otherwise, ptr must come immediately after the block header
        else {
            if (reinterpret_cast<uintptr_t>(blockHeader) + sizeof(AllocatedMemoryBlockHeader) != ptrAddr)
                return nullptr;
        }
        return blockHeader;
    }

    bool MemorySpanHeader::freeBlock(void* ptr, SPAN_ALLOC_STAT_LIST) {
        //First, we check that the pointer is indeed valid to free. We find the header of the block we believe to contain
        //ptr by finding ptr's floor in allocatedBlockTree. This gives us the closest AllocatedMemoryBlockHeader to ptr.
        AllocatedMemoryBlockHeader* blockHeader = getValidatedHeaderForPtr(ptr);
        if (blockHeader == nullptr) return false;
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        requestedAllocationStat -= blockHeader -> requestedSize;
#endif
        committedAllocationStat -= blockHeader -> size();
        //If we've made it to this point, we've confirmed the free was to a valid pointer, and we get the address of the
        //beginning of its block.
        const auto blockAddr = reinterpret_cast<uintptr_t>(blockHeader);
        //Then we remove it from the allocated block tree, preventing double-frees, for example. We are sure to retain
        //the size of the block before erasing it. Strictly speaking, the order should not be necessary, but I think it
        //would be wise to treat the header as invalid after calling erase.
        const uint32_t size = blockHeader->size();
        allocatedBlockTree.template erase<StaticStack<AllocatedMemoryBlockHeader**, 64>>(blockHeader);
        //We now create a new free header at the start of the block and populate it with the correct size
        auto* freeHeader = reinterpret_cast<UnallocatedMemoryBlockHeader*>(blockAddr);
        freeHeader->sizeAndColor = size;
        //Then we add the free block to the free trees, and coalesce any adjacent free blocks.
        insertFreeBlock(freeHeader);
        coalesceAdjacentFreeBlocks(freeHeader);
        //Finally, we update our record of the span's total free space.
        freeSpace += size;
        return true;
    }

    bool MemorySpanHeader::isPointerAllocated(void *ptr) const {
        return getValidatedHeaderForPtr(ptr) != nullptr;
    }

    struct MemorySpanFreeSpaceInfoExtractor {
        static MemorySpanHeader*& left(MemorySpanHeader& header){return header.unallocatedTreeLeftChild;}
        static MemorySpanHeader*& right(MemorySpanHeader& header){return header.unallocatedTreeRightChild;}
        static MemorySpanHeader* const& left(const MemorySpanHeader& header){return header.unallocatedTreeLeftChild;}
        static MemorySpanHeader* const& right(const MemorySpanHeader& header){return header.unallocatedTreeRightChild;}
        static MemorySpanHeader*& parent(MemorySpanHeader& header){return header.unallocatedTreeParent;}
        static MemorySpanHeader* const& parent(const MemorySpanHeader& header){return header.unallocatedTreeParent;}
        static MemorySpanHeader& data(MemorySpanHeader& header) {return header;}
        static const MemorySpanHeader& data(const MemorySpanHeader& header) {return header;}
        static bool isRed(MemorySpanHeader& header){return header.flags.unallocatedTreeColor;}
        static void setRed(MemorySpanHeader& header, bool red){header.flags.unallocatedTreeColor = red;}
        static size_t& augmentedData(MemorySpanHeader& header){return header.largestFreeBlockInMallocSubtree;}
        static size_t recomputeAugmentedData(const MemorySpanHeader& header, const MemorySpanHeader* left, const MemorySpanHeader* right) {
            const size_t size = header.largestFreeBlockSize;
            const size_t leftSize = left ? left->largestFreeBlockInMallocSubtree : 0;
            const size_t rightSize = right ? right->largestFreeBlockInMallocSubtree : 0;
            return max(size, leftSize, rightSize);
        }
    };

    struct MemorySpanAddressInfoExtractor {
        static MemorySpanHeader*& left(MemorySpanHeader& header){return header.allocatedTreeLeftChild;}
        static MemorySpanHeader*& right(MemorySpanHeader& header){return header.allocatedTreeRightChild;}
        static MemorySpanHeader* const& left(const MemorySpanHeader& header){return header.allocatedTreeLeftChild;}
        static MemorySpanHeader* const& right(const MemorySpanHeader& header){return header.allocatedTreeRightChild;}
        static MemorySpanHeader*& parent(MemorySpanHeader& header){return header.allocatedTreeParent;}
        static MemorySpanHeader* const& parent(const MemorySpanHeader& header){return header.allocatedTreeParent;}
        static uintptr_t data(const MemorySpanHeader& header) {return reinterpret_cast<uintptr_t>(&header);}
        static bool isRed(MemorySpanHeader& header){return header.flags.allocatedTreeColor;}
        static void setRed(MemorySpanHeader& header, bool red){header.flags.allocatedTreeColor = red;}
    };

    //When looking up spans to allocate a new block, we first order by remaining free space and then
    //use the addresses of the spans to break any ties.
    struct MemorySpanUnallocatedComparator {
        bool operator()(const MemorySpanHeader& a, const MemorySpanHeader& b) const {
            if (a.freeSpace == b.freeSpace) {
                return reinterpret_cast<uintptr_t>(&a) < reinterpret_cast<uintptr_t>(&b);
            }
            return a.freeSpace < b.freeSpace;
        }
    };

    struct CoarseAllocatorStatistics {
        size_t totalSystemMemoryAllocated;
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        size_t totalBytesRequested;
#endif
        size_t totalBytesInAllocatedBlocks;
        size_t totalSizeOfSpanHeaders;

        CoarseAllocatorStatistics() :
        totalSystemMemoryAllocated(0),
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        totalBytesRequested(0),
#endif
        totalBytesInAllocatedBlocks(0), totalSizeOfSpanHeaders(0)
        {}
    };

    class CoarseInternalAllocator : public Allocator{
    private:
        CoarseAllocatorStatistics stats;
        friend void validateAllocatorIntegrity();
        friend size_t computeTotalAllocatedSpaceInCoarseAllocator();
        friend size_t computeTotalFreeSpaceInCoarseAllocator();
        friend bool isValidPointer(void* ptr);
        IntrusiveRedBlackTree<MemorySpanHeader, MemorySpanFreeSpaceInfoExtractor, MemorySpanUnallocatedComparator> spansByFreeSpace;
        IntrusiveRedBlackTree<MemorySpanHeader, MemorySpanAddressInfoExtractor> spansByAddress;

        MemorySpanHeader* findSpanContaining(void* ptr);
        MemorySpanHeader* findMostOccupiedSpanFittingRequest(size_t size, std::align_val_t align);
        void destroySpan(MemorySpanHeader* span);
    public:
        ~CoarseInternalAllocator() override = default;
        MemorySpanHeader* createSpan(size_t spanSize, void* baseAddr);
        void* allocate(size_t size, std::align_val_t align) override;
        bool free(void* ptr) override;
        CoarseAllocatorStatistics getStatistics() const;
    };

    CoarseAllocatorStatistics CoarseInternalAllocator::getStatistics() const {
        return stats;
    }

    MemorySpanHeader* CoarseInternalAllocator::findSpanContaining(void* ptr) {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        //This is our best candidate span
        auto header = this -> spansByAddress.floor(addr);
        //If the span doesn't exist, bail
        if (header == nullptr) return nullptr;
        //If ptr isn't contained in the span, return null
        if (addr >= reinterpret_cast<uintptr_t>(header) + header -> spanSize) return nullptr;
        return header;
    }

    MemorySpanHeader* CoarseInternalAllocator::findMostOccupiedSpanFittingRequest(size_t size, std::align_val_t align) {
        //Compute the minimum size free block needed to guarantee we can fulfill the request
        size_t paddedSize = computeWorstCaseAlignedSize(size, align);

        MemorySpanHeader* best = nullptr;
        auto current = spansByFreeSpace.getRoot();
        //Computes min() for the subtree of spans containing a suitably large free block.
        while (current != nullptr) {
            //If the subtree rooted at current can't fit the request, we're done
            if (current -> largestFreeBlockInMallocSubtree < paddedSize) break;
            auto* leftChild = current -> unallocatedTreeLeftChild;
            auto* rightChild = current -> unallocatedTreeRightChild;
            //If current can fit the request, update our best pointer and see if we can do better traversing to the left
            if (paddedSize <= current -> largestFreeBlockSize) {
                best = current;
                current = leftChild;
                continue;
            }
            //If current can't fit the request, but the subtree rooted at the left child can, then search in the left subtree
            if (leftChild != nullptr && leftChild -> largestFreeBlockInMallocSubtree >= paddedSize) {
                current = leftChild;
                continue;
            }
            //If all else fails, the right subtree has a span that can fulfill the request
            current = rightChild;
        }

        return best;
    }

    void CoarseInternalAllocator::destroySpan(MemorySpanHeader* span) {
        //If we initialized the allocator with a fixed buffer, such as in the kernel, then we shouldn't try to release it.
        if (!span -> flags.releasable) return;
        this -> spansByFreeSpace.erase(span);
        this -> spansByAddress.erase(span);

        stats.totalSystemMemoryAllocated -= span -> spanSize;
        stats.totalSizeOfSpanHeaders -= sizeof(MemorySpanHeader);

        using namespace LibAlloc::Backend;
        freePages(span, (span -> spanSize)/smallPageSize);
    }

    MemorySpanHeader* CoarseInternalAllocator::createSpan(size_t spanSize, void* baseAddr) {
        auto* span = new (baseAddr) MemorySpanHeader(spanSize);
        this -> spansByFreeSpace.insert(span);
        this -> spansByAddress.insert(span);
        stats.totalSystemMemoryAllocated += spanSize;
        stats.totalSizeOfSpanHeaders += sizeof(MemorySpanHeader);
        return span;
    }

#ifdef TRACK_REQUESTED_ALLOCATION_STATS
#define COARSE_ALLOCATOR_SPAN_STATS stats.totalBytesRequested, stats.totalBytesInAllocatedBlocks
#else
#define COARSE_ALLOCATOR_SPAN_STATS stats.totalBytesInAllocatedBlocks
#endif

    bool CoarseInternalAllocator::free(void* ptr) {
        auto* span = findSpanContaining(ptr);
        if (span == nullptr) return false;
        bool out = false;
        //Automatically propagates changes to the augmentation data. Note that only spansByFreeSpace is augmented,
        //and spansByAddress isn't. Furthermore, modifying a span does not change its base address, so the structure
        //of the spansByAddress tree does not need to change.
        spansByFreeSpace.update(span, [ptr, &out, this](MemorySpanHeader& header) {
           out = header.freeBlock(ptr, COARSE_ALLOCATOR_SPAN_STATS);
        });

        if (span -> freeSpace == span -> getBufferSize()) {
            //should we delay destroying the span for a little to avoid thrashing?
            destroySpan(span);
        }
        return out;
    }

    constexpr size_t minimumSpanSize = 16 * 1024;

    void *CoarseInternalAllocator::allocate(size_t size, std::align_val_t align) {
        auto* span = findMostOccupiedSpanFittingRequest(size, align);
        if (span == nullptr) {
            using namespace LibAlloc::Backend;
            //If we couldn't find a span that could fit the request, we need to create a new span.
            //We need to find the smallest span that can fit the request, and then create a new span that is
            //at least as large as the request.
            size_t paddedSize = computeWorstCaseAlignedSize(size, align);
            size_t spanSize = roundUpToNearestMultiple(max(2 * paddedSize + sizeof(MemorySpanHeader), minimumSpanSize), smallPageSize);
            auto spanStart = allocPages(spanSize/smallPageSize);
            assert(spanStart != nullptr, "Failed to allocate memory for new span");
            createSpan(spanSize, spanStart);
            span = findMostOccupiedSpanFittingRequest(size, align);
            assert(span != nullptr, "Failed to create new span");
            assert(span -> spanSize >= paddedSize, "New span has unexpected size");
            assert(reinterpret_cast<uintptr_t>(span) % smallPageSize == 0, "New span not page aligned");
        }
        void* out = nullptr;
        //Automatically propagates changes to the augmentation data. Note that only spansByFreeSpace is augmented,
        //and spansByAddress isn't. Furthermore, modifying a span does not change its base address, so the structure
        //of the spansByAddress tree does not need to change.
        spansByFreeSpace.update(span, [size, align, &out, this](MemorySpanHeader& header) {
            out = header.allocateBlock(size, align, COARSE_ALLOCATOR_SPAN_STATS);
        });
        return out;
    }

#define ALLOW_ZERO_ALLOC

    class InternalAllocator : public LibAlloc::Allocator {
        friend void validateAllocatorIntegrity();
        friend size_t computeTotalAllocatedSpaceInCoarseAllocator();
        friend size_t computeTotalFreeSpaceInCoarseAllocator();
        friend bool isValidPointer(void* ptr);
        friend InternalAllocatorStats getAllocatorStats();

        CoarseInternalAllocator coarseAllocator;
        SlabTreeType slabTree;
        ConstexprArray<SlabAllocator, slabSizeClasses.size()> slabAllocators;
#ifdef ALLOW_ZERO_ALLOC
        uint8_t zero;
#endif
        template <size_t... Is>
        ConstexprArray<SlabAllocator, slabSizeClasses.size()> makeSlabAllocators(index_sequence<Is...>);
        friend size_t getInternalAllocRemainingSlabCount();
    public:
        InternalAllocator(void* initialBuffer, size_t size);
        InternalAllocator();
        void grantBuffer(void* buffer, size_t size);
        void* allocate(size_t size, std::align_val_t align) override;
        bool free(void* ptr) override;
    };

    template <size_t... Is>
    ConstexprArray<SlabAllocator, slabSizeClasses.size()> InternalAllocator::makeSlabAllocators(index_sequence<Is...> _) {
        (void)_;
        return {SlabAllocator(slabSizeClasses[Is], slabAllocatorBufferSizes[Is], this -> coarseAllocator, this -> slabTree)...};
    }

    InternalAllocator::InternalAllocator() : slabAllocators(makeSlabAllocators(make_index_sequence<slabSizeClasses.size()>{})) {
        zero = 0;
    }

    InternalAllocator::InternalAllocator(void* initialBuffer, size_t size) : slabAllocators(makeSlabAllocators(make_index_sequence<slabSizeClasses.size()>{})) {
        auto* span = coarseAllocator.createSpan(size, initialBuffer);
        span -> markUnreleasable();
        zero = 0;
    }

    void* InternalAllocator::allocate(size_t size, std::align_val_t align) {
#ifdef ALLOW_ZERO_ALLOC
        if (condition_unlikely(size == 0)) {
            return &zero;
        }
#endif
        constexpr size_t maxSlabSize = slabSizeClasses[slabSizeClasses.size() - 1];
        if (size > maxSlabSize) {
            return coarseAllocator.allocate(size, align);
        }
        size_t slabIndex = sizeClassIndex<slabSizeClasses>(size);
        auto alignVal = static_cast<size_t>(align);
#ifdef ASSUME_ALIGN_POWER_OF_TWO
        if (condition_likely((slabSizeClasses[slabIndex] & (alignVal - 1)) == 0)) {
            return slabAllocators[slabIndex].alloc();
        }
        size_t alignedSize = max(2 << log2floor(size), alignVal);
        if (condition_likely(alignedSize <= maxSlabSize)) {
            slabIndex = sizeClassIndex<slabSizeClasses>(alignedSize);
            if ((slabSizeClasses[slabIndex] & (alignVal - 1)) == 0)
                return slabAllocators[slabIndex].alloc();
        }
#else
        if (condition_likely(slabSizeClasses[slabIndex] % alignVal == 0)) {
            return slabAllocators[slabIndex].alloc();
        }
#endif
        return coarseAllocator.allocate(size, align);
    }

    bool InternalAllocator::free(void *ptr) {
#ifdef ALLOW_ZERO_ALLOC
        if (condition_unlikely(ptr == &zero)) {
            return true;
        }
#endif
        //I think the C spec says freeing null is allowed
        if (condition_unlikely(ptr == nullptr)) {
            return true;
        }

        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        auto potentialSlab = slabTree.floor(addr);
        if (potentialSlab != nullptr) {
            if (potentialSlab -> contains(ptr)) {
                if (condition_unlikely(!potentialSlab -> containsWithAlignment(ptr))) return false;
                potentialSlab -> getAllocator() -> free(ptr, *potentialSlab);
                return true;
            }
        }

        return coarseAllocator.free(ptr);
    }

    void InternalAllocator::grantBuffer(void *buffer, size_t size) {
        auto* span = this -> coarseAllocator.createSpan(size, buffer);
        span -> markUnreleasable();
    }

    InternalAllocator internalAllocator;

    void initializeInternalAllocator() {
        new(&internalAllocator) InternalAllocator();
    }

    void* malloc(size_t size, std::align_val_t align) {
        return internalAllocator.allocate(size, align);
    }

    void free(void* ptr) {
        assert(internalAllocator.free(ptr), "Tried to free invalid pointer");
    }

    void validateNoAdjacentFreeBlocks(MemorySpanHeader& span) {
        span.unallocatedBlocksByAddress.visitDepthFirstInOrder([&](UnallocatedMemoryBlockHeader& header){
            UnallocatedMemoryBlockHeader* successor = span.unallocatedBlocksByAddress.successor(&header);
            auto successorAddr = reinterpret_cast<uintptr_t>(successor);
            auto headerAddr = reinterpret_cast<uintptr_t>(&header);
            assert(successorAddr != headerAddr + header.size(), "Adjacent free blocks found");
        });
    }

    size_t totalFreeBlockSize(MemorySpanHeader& span) {
        size_t out = 0;
        span.unallocatedBlocksByAddress.visitDepthFirstInOrder([&](UnallocatedMemoryBlockHeader& header){
            out += header.size();
        });
        return out;
    }

    size_t totalAllocatedBlockSize(MemorySpanHeader& span) {
        size_t out = 0;
        span.allocatedBlockTree.visitDepthFirstInOrder([&](AllocatedMemoryBlockHeader& header){
            out += header.size();
        });
        return out;
    }

    void validateSpan(MemorySpanHeader& span) {
        validateNoAdjacentFreeBlocks(span);
        const auto totalFree = totalFreeBlockSize(span);
        const auto totalAllocated = totalAllocatedBlockSize(span);
        assert(totalFree == span.freeSpace, "Free block size does not match free space");
        assert(totalFree + totalAllocated == span.getBufferSize(), "Allocated block size does not match span size");
    }

    void validateAllocatorIntegrity() {
        internalAllocator.coarseAllocator.spansByAddress.visitDepthFirstInOrder([](MemorySpanHeader& header){
            validateSpan(header);
        });
    }

    size_t computeTotalAllocatedSpaceInCoarseAllocator() {
        for (auto& slab : internalAllocator.slabAllocators) {
            slab.releaseAllFreeSlabs();
        }
        size_t out = 0;
        internalAllocator.coarseAllocator.spansByAddress.visitDepthFirstInOrder([&](MemorySpanHeader& header){
            out += totalAllocatedBlockSize(header);
        });
        /*for (auto& slab : internalAllocator.slabAllocators) {
            auto stats = slab.getStatistics();
            out -= stats.totalBackingSize;
            out += stats.currentlyAllocatedSize;
        }*/
        return out;
    }

    size_t computeTotalFreeSpaceInCoarseAllocator() {
        for (auto& slab : internalAllocator.slabAllocators) {
            slab.releaseAllFreeSlabs();
        }
        size_t out = 0;
        internalAllocator.coarseAllocator.spansByAddress.visitDepthFirstInOrder([&](MemorySpanHeader& header){
            out += totalFreeBlockSize(header);
        });
        /*for (auto& slab : internalAllocator.slabAllocators) {
            auto stats = slab.getStatistics();
            out += stats.totalBackingSize;
            out -= stats.currentlyAllocatedSize;
        }*/
        return out;
    }

    bool isValidPointer(void *ptr) {
        auto* slab = internalAllocator.slabTree.floor(reinterpret_cast<uintptr_t>(ptr));
        if (slab != nullptr) {
            if (slab -> containsWithAlignment(ptr) && !slab -> isFree(ptr)) {
                return true;
            }
        }
        auto* span = internalAllocator.coarseAllocator.findSpanContaining(ptr);
        if (span == nullptr) return false;
        return span -> isPointerAllocated(ptr);
    }

    InternalAllocatorStats getAllocatorStats() {
        for (auto& slab : internalAllocator.slabAllocators) {
            slab.releaseAllFreeSlabs();
        }
        InternalAllocatorStats out{};
        const auto coarseStats = internalAllocator.coarseAllocator.getStatistics();
        out.totalSystemMemoryAllocated = coarseStats.totalSystemMemoryAllocated;
#ifdef TRACK_REQUESTED_ALLOCATION_STATS
        out.totalBytesRequested = coarseStats.totalBytesRequested;
#endif
        out.totalUsedBytesInAllocator = coarseStats.totalSizeOfSpanHeaders + coarseStats.totalBytesInAllocatedBlocks;
        return out;
    }

#ifdef TRACK_REQUESTED_ALLOCATION_STATS
    size_t InternalAllocatorStats::computeAllocatorMetadataOverhead() const{
        return totalUsedBytesInAllocator - totalBytesRequested;
    }

    /*float InternalAllocatorStats::computeAllocatorMetadataPercentOverhead() const {
        return static_cast<float>(computeAllocatorMetadataOverhead()) / static_cast<float>(totalUsedBytesInAllocator);
    }*/

    size_t getInternalAllocRemainingSlabCount() {
        for (auto& slab : internalAllocator.slabAllocators) {
            slab.releaseAllFreeSlabs();
        }
        size_t out = 0;
        internalAllocator.slabTree.visitDepthFirstInOrder([&](auto _) {
            (void)_;
            out++;
        });
        return out;
    }

	void grantBuffer(void* buffer, size_t size) {
        internalAllocator.grantBuffer(buffer, size);
    }

#endif
}