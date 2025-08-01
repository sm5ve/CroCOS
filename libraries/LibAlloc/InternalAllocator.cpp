//
// Created by Spencer Martin on 8/1/25.
//

#include <stddef.h>
#include <Backend.h>

/*
*Okay, so I think I have an idea for how to start, and I’d like to walk through this with you.
*Namely, the allocator will have 2 layers: a simple and less efficient “internal” allocator,
*and a more sophisticated external allocator that should be more memory efficient and experience
*less lock contention.
*
*Although the internal allocator is simpler, I think it would still be a little involved, so bear with me.
*At this lowest level, the only sort of memory allocator we have at our disposal is whatever backend method
*provides us with pages - so mmap in userspace and the page allocator + virtual memory manager in kernel mode.
*We need to store metadata and data together in these pages, so that way the allocator structure itself can be
*of constant size - it will just read into these dynamically generated data structures in the newly allocated pages.
*Let’s refer to a contiguous (in the virtual address space) range of pages in this context as a “Span”. We want to
*make two data structures out of these spans: first, a heap to facilitate finding a good (hopefully densely packed)
*span to fit an allocation, and a BST to quickly find the right span to free a block of memory from. The data to
*encode these tree structures will be maintained in small header structs at the beginning of each span, so the
*internal allocator only needs to maintain pointers to the roots of these structures. These span headers will also
*keep track of the size of each span.
*
*Within a span, we will have similar data structures. We’ll split a span into chunks of memory to be allocated,
*with each chunk starting with a header indicating its size and some other metadata. Among that metadata will be
*the necessary pointers to child nodes for intra-span BSTs and heaps, serving a similar function to the broader
*inter-heap structures. The memory blocks will also store whether they are occupied and encode a doubly
*linked list to adjacent memory blocks to allow for coalescing empty blocks into bigger ones.
*
*This system has a fairly hefty overhead for small allocations. Thus, building on top of this, we will allocate
*smaller internal slab allocators inside the general internal memory allocator and defer small memory allocations
*to the slab allocators. When a slab allocator is emptied, the allocator itself will be freed. The whole system
*will be behind a global lock (unless there’s a clever way to make this lockless, which I highly doubt, though
*the slab allocators might be amenable to lockless operation with atomic CAS) and we could implement a caching
*system on top of this to further reduce lock contention
*/

struct BlockHeader;

struct SpanHeader {
    size_t spanSize;
    size_t maximumBlockSize;
    size_t totalFreeSpace;
    BlockHeader* allocBSTRoot;
    BlockHeader* freeHeapRoot;
    SpanHeader* bstLeft;
    SpanHeader* bstRight;
    SpanHeader* heapLeft;
    SpanHeader* heapRight;
};

struct BlockHeader {
    size_t blockSize;
    bool isFree;
    BlockHeader* prevByAddr;
    BlockHeader* nextByAddr;
    BlockHeader* treeLeft;
    BlockHeader* treeRight;
};

SpanHeader* allocateAndInitializeSpan(size_t npages) {

}