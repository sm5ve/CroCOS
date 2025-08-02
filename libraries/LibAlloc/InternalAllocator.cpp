//
// Created by Spencer Martin on 8/1/25.
//

#include <stddef.h>
#include <Backend.h>
#include <PointerArithmetic.h>

struct BlockHeader;

struct alignas(16) SpanHeader {
    //Size of entire span including header
    size_t spanSize;
    size_t maximumFreeBlockSize;
    size_t totalFreeSpace;
    bool isPermanent;
    bool isRed;
    BlockHeader* internalAllocBSTRoot;
    BlockHeader* internalFreeBSTRoot;
    //SpanHeader* allocBSTParent;
    SpanHeader* allocBSTLeft;
    SpanHeader* allocBSTRight;
    //SpanHeader* freeBSTParent;
    SpanHeader* freeBSTLeft;
    SpanHeader* freeBSTRight;
};

static_assert(sizeof(SpanHeader) % 16 == 0);

struct alignas(16) BlockHeader {
    //Size excluding header
    size_t blockSize;
    bool isFree;
    bool isRed;
    BlockHeader* prevByAddr;
    BlockHeader* nextByAddr;
    BlockHeader* bstLeft;
    BlockHeader* bstRight;
};

static_assert(sizeof(BlockHeader) % 16 == 0);

BlockHeader& firstBlockForSpan(SpanHeader& header) {
    //The first block header is always immediately after the span header
    return *offsetPointerByBytes<BlockHeader>(&header, sizeof(SpanHeader));
}

void initializeSpan(SpanHeader& header, size_t npages) {
    //first initialize the block header
    auto& firstBlock = firstBlockForSpan(header);
    firstBlock.blockSize = npages * LibAlloc::Backend::smallPageSize - sizeof(SpanHeader) - sizeof(BlockHeader);
    firstBlock.isFree = true;
    firstBlock.prevByAddr = nullptr;
    firstBlock.nextByAddr = nullptr;
    firstBlock.bstLeft = nullptr;
    firstBlock.bstRight = nullptr;
    firstBlock.isRed = false;
    //initialize span header
    header.spanSize = npages * LibAlloc::Backend::smallPageSize;
    header.maximumFreeBlockSize = firstBlock.blockSize;
    header.totalFreeSpace = firstBlock.blockSize;
    header.internalAllocBSTRoot = nullptr;
    header.internalFreeBSTRoot = &firstBlock;
    header.allocBSTLeft = nullptr;
    header.allocBSTRight = nullptr;
    //header.allocBSTParent = nullptr;
    header.freeBSTLeft = nullptr;
    header.freeBSTRight = nullptr;
    //header.freeBSTParent = nullptr;
    header.isPermanent = false;
    header.isRed = false;
}

SpanHeader& allocateAndInitializeSpan(const size_t npages) {
    auto& out = *static_cast<SpanHeader*>(LibAlloc::Backend::allocPages(npages));
    initializeSpan(out, npages);
    return out;
}

void* allocateFromSpan(SpanHeader& span, const size_t size) {
    //If the span doesn't report that it has a free block of sufficient size, abort
    if (span.maximumFreeBlockSize < size) {
        return nullptr;
    }
    //Otherwise,

}