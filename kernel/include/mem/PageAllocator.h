//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATOR_H
#define CROCOS_PAGEALLOCATOR_H

#include <mem/MemTypes.h>
#include <arch.h>
#include <core/utility.h>
#include <core/atomic.h>
#include <core/atomic/RingBuffer.h>
#include <core/atomic/HighReliabilityRingBuffer.h>
#include <mem/NUMA.h>
#include <core/atomic/AtomicBitPool.h>

#include <mem/mm.h>
#include <core/Flags.h>

// ==================== Allocation Behavior Flags ====================

enum class AllocBehavior : uint32_t {
    BIG_PAGE_ONLY   = 1u << 0,  // Only allocate big (2MiB) pages; never fall back to small pages
    LOCAL_DOMAIN_ONLY = 1u << 1,  // Only allocate from the calling CPU's local pool; never go to NUMA pool
    GRACEFUL_OOM   = 1u << 2,
};
template<> struct is_flags_enum<AllocBehavior> { static constexpr bool value = true; };
using AllocFlags = Flags<AllocBehavior>;

// ==================== Struct Definitions ====================

class BootstrapAllocator {
    uint8_t* current;
    uint8_t* end;
    bool measuring;

public:
    // Dry-run constructor (measuring mode)
    BootstrapAllocator();
    // The actual allocator initializer
    BootstrapAllocator(void* buffer, size_t size);

    template<typename T>
    T* allocate(size_t count, size_t alignment = alignof(T));
    template<typename T>
    T* allocate();
    template<typename T>
    T* allocate(FunctionRef<void(T&)> init, size_t count, size_t alignment = alignof(T));
    [[nodiscard]] size_t bytesNeeded() const;
    [[nodiscard]] size_t bytesRemaining() const;
    [[nodiscard]] bool isFake() const {return measuring;}
};

struct PageRef {
    uint64_t value;

    static PageRef small(kernel::mm::phys_addr addr);
    static PageRef big(kernel::mm::phys_addr addr);

    [[nodiscard]] kernel::mm::PageSize size() const;
    [[nodiscard]] kernel::mm::phys_addr addr() const;

    bool operator==(const PageRef& other) const {return other.value == value;}
} __attribute__((packed));

constexpr auto INVALID_PAGE_REF = PageRef{static_cast<uint64_t>(-1)};

using PageAllocationCallback = FunctionRef<void(PageRef)>;

// ==================== Occupancy Transition ====================

enum class OccupancyState : uint8_t { Empty, Partial, Full };

struct OccupancyTransition {
    OccupancyState before;
    OccupancyState after;

    [[nodiscard]] bool becameFull()      const { return before != OccupancyState::Full  && after == OccupancyState::Full; }
    [[nodiscard]] bool becameEmpty()     const { return before != OccupancyState::Empty && after == OccupancyState::Empty; }
    [[nodiscard]] bool becameAvailable() const { return before == OccupancyState::Full  && after != OccupancyState::Full; }
};

// ==================== Small Page Allocator ====================

class SmallPageAllocator {
    friend class BigPageMetadata;
    using SmallPageIndex = SmallestUInt_t<log2ceil(kernel::mm::PageAllocator::smallPagesPerBigPage)>;
    using SmallPageCount = SmallestUInt_t<log2ceil(kernel::mm::PageAllocator::smallPagesPerBigPage + 1)>;
    constexpr static size_t bitmapWordCount = kernel::mm::PageAllocator::smallPagesPerBigPage / (8 * sizeof(uint64_t));

    // bit=1: page is available for the current alloc CPU to hand out.
    // Non-atomic: only ever touched by the one CPU that currently owns this big page
    // for allocation (either a LocalPool CPU or a single NUMAPool allocating thread).
    alignas(64) uint64_t allocBitmap[bitmapWordCount];
    // bit=1: page has been freed by any CPU and is waiting to be picked up.
    // Atomic: written concurrently by any freeing CPU; drained exclusively by the alloc CPU.
    alignas(64) Atomic<uint64_t> freeBitmap[bitmapWordCount]{};

    kernel::mm::phys_addr baseAddr;
    SmallPageCount reservedCount = 0;
    Atomic<SmallPageCount> allocatedCount = 0;
    // First allocBitmap word index that might be non-zero. Non-atomic: alloc CPU only.
    uint8_t allocHint = 0;

    [[nodiscard]] kernel::mm::phys_addr fromPageIndex(SmallPageIndex index) const;
    [[nodiscard]] static OccupancyState stateFromCount(size_t count, size_t maxAlloc);
    // Flush any remaining allocBitmap pages into freeBitmap before handing this
    // big page back to the NUMAPool, so the next alloc CPU finds them in freeBitmap.
    void flushAllocBitmap();

public:
    explicit SmallPageAllocator(kernel::mm::phys_addr base);

    [[nodiscard]] bool isPageFree(PageRef page) const;
    size_t alloc(PageAllocationCallback cb, size_t count, OccupancyTransition& transition);
    void free(PageRef* pages, size_t count, OccupancyTransition& transition);
    size_t alloc(PageAllocationCallback cb, size_t count) { OccupancyTransition t; return alloc(cb, count, t); }
    void free(PageRef* pages, size_t count) { OccupancyTransition t; free(pages, count, t); }
    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] bool hasReservedPages() const { return reservedCount > 0; }
    [[nodiscard]] size_t freePageCount() const;
    void freeAll();
    void allocAll();
    void reservePage(kernel::mm::phys_addr addr);

#ifdef CROCOS_TESTING
    [[nodiscard]] size_t getAllocatedCount() const { return static_cast<size_t>(allocatedCount.load(RELAXED)); }
    [[nodiscard]] size_t getReservedCount()  const { return reservedCount; }
    // Count of pages currently in allocBitmap or freeBitmap (i.e. free and not reserved).
    // Safe to call only in quiescent (single-threaded) state.
    [[nodiscard]] size_t getBitmapPopcount() const;
    // Invariant: bitmapPopcount + allocatedCount + reservedCount == smallPagesPerBigPage.
    [[nodiscard]] bool   checkInvariants()   const;
#endif
};

// ==================== Big Page Metadata ====================

class NUMAPool;

class alignas(64) BigPageMetadata {
    [[no_unique_address]] SmallPageAllocator subpageAllocator;
    NUMAPool* ownerPool;
    // Hint: true when this big page is currently cached in a LocalPool.
    // Written by the LocalPool CPU (set on accept, cleared in returnPage).
    // Read by freeing CPUs in NUMAPool::freePages to skip a paPages.remove()
    // that is guaranteed to return NotPresent anyway.
    Atomic<bool> inLocalPool{false};

public:
    BigPageMetadata(NUMAPool& pool, kernel::mm::phys_addr baseAddr);

    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, OccupancyTransition& transition);
    void freePages(PageRef* pages, size_t count, OccupancyTransition& transition);
    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb) { OccupancyTransition t; return allocatePages(smallPageCount, cb, t); }
    void freePages(PageRef* pages, size_t count) { OccupancyTransition t; freePages(pages, count, t); }

    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isEmpty() const;

    void allocAll() {subpageAllocator.allocAll();}
    void freeAll() {subpageAllocator.freeAll();}

    // Reserve a small page so it is never handed to callers (init-time only).
    void reservePage(kernel::mm::phys_addr addr) { subpageAllocator.reservePage(addr); }
    [[nodiscard]] bool hasReservedSubpages() const { return subpageAllocator.hasReservedPages(); }

    [[nodiscard]] NUMAPool& getOwnerPool() const { return *ownerPool; }
    void returnPage();
    [[nodiscard]] kernel::mm::phys_addr baseAddr() const {return subpageAllocator.baseAddr;}

    void markInLocalPool()  { inLocalPool.store(true,  RELEASE); }
    [[nodiscard]] bool isInLocalPool() const { return inLocalPool.load(ACQUIRE); }

#ifdef CROCOS_TESTING
    // Number of subpages that are currently free (not allocated or reserved).
    [[nodiscard]] size_t freeSubpageCount() const { return subpageAllocator.freePageCount(); }
    // True when the given small-page ref is currently allocated in this big page.
    [[nodiscard]] bool isSubpageAllocated(PageRef page) const { return !subpageAllocator.isPageFree(page); }
#endif
};

struct SubrangeInfo {
    kernel::mm::phys_addr rangeStart;  // big-page-aligned start of merged range
    kernel::mm::phys_addr rangeEnd;    // big-page-aligned end of merged range
    BigPageMetadata* metadataBase;     // pointer to first BigPageMetadata for this range
};

class NUMAPool {
    BigPageMetadata* bigPageMetadataBuffer;
    HighReliabilityRingBuffer<BigPageMetadata*, false, true> freeBigPages;
    AtomicBitPool paPages;
    kernel::numa::DomainID associatedDomain;
    SubrangeInfo* subrangeInfo;
    size_t subrangeCount;
    size_t bigPageCount;

    void fixupAfterReserveRange();
public:
    NUMAPool(BigPageMetadata* metadataBuffer,
             BigPageMetadata** freeBuffer,
             Atomic<size_t>* wgc,
            Atomic<size_t>* rgc,
             AtomicBitPool&& paPagesBitPool,
             SubrangeInfo* subrangeBuffer,
             size_t numSubranges,
             size_t totalBigPageCount,
             kernel::numa::DomainID domain);

    // Returns the BigPageMetadata for the big page containing addr,
    // or nullptr if addr is not within any subrange of this pool.
    BigPageMetadata* findMetadata(kernel::mm::phys_addr addr);

    // Returns the pool-global index of a BigPageMetadata (its position in metadataBuffer).
    size_t metadataIndex(const BigPageMetadata* meta) const { return static_cast<size_t>(meta - bigPageMetadataBuffer); }

    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, BigPageMetadata*& paPageRemaining, AllocFlags flags = {});
    void freePages(PageRef* pages, size_t count);
    void returnPage(BigPageMetadata& metadata);

    // Reserve all small pages within range that fall in this pool.
    // Must be called before any allocation (init-time only).
    void reserveRange(kernel::mm::phys_memory_range range);

    kernel::numa::DomainID domain() const { return associatedDomain; }

    const SubrangeInfo* getSubranges()    const { return subrangeInfo; }
    size_t              getSubrangeCount() const { return subrangeCount; }

#ifdef CROCOS_TESTING
    // Number of free big pages currently in the freeBigPages ring buffer.
    [[nodiscard]] size_t getFreeBigPageCount() const { return freeBigPages.availableToRead(); }
    // Number of indices currently set in paPages.  Quiescent state only.
    [[nodiscard]] size_t getPAPagesCount()     const { return paPages.countSet(); }
    [[nodiscard]] size_t getTotalBigPageCount()const { return bigPageCount; }
    // Returns true if the given pool-global big-page index is currently in paPages.
    [[nodiscard]] bool   isInPAPages(size_t i) const { return paPages.isSet(i); }
    // Quiescent-state invariant check: all paPages entries must be partial pages.
    [[nodiscard]] bool   checkInvariants()     const;
    // Sum of freeSubpageCount() across every BigPageMetadata in this pool.
    // Correctly accounts for pages in freeBigPages, paPages, or cached in a LocalPool.
    [[nodiscard]] size_t countTotalFreePages() const;
#endif
};

class LocalPool {
    BigPageMetadata* paPage1 = nullptr;
    BigPageMetadata* paPage2 = nullptr;
    const kernel::numa::NUMATopology* topology;
    NUMAPool* homePool = nullptr;
public:
    explicit LocalPool(const kernel::numa::NUMATopology* topo = nullptr, NUMAPool* home = nullptr)
        : topology(topo), homePool(home) {}

    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags = {});
    void tryGivePAPage(BigPageMetadata& page);
};

// ==================== New Page Allocator ====================

// One entry per SubrangeInfo (merged range) across all NUMA domains.
// Sorted by rangeStart for binary search in findMetadata.
struct NUMADomainEntry {
    kernel::mm::phys_addr rangeStart;
    kernel::mm::phys_addr rangeEnd;
    NUMAPool* pool;
};

struct PageAllocatorImpl {
    NUMAPool** numaPools;
    size_t numDomains;
    LocalPool** localPools;
    NUMADomainEntry* domainTable;
    size_t domainTableSize;
    // Physical memory with no NUMA affinity; queried last during allocation.
    // nullptr when no unowned ranges exist.
    NUMAPool* unownedPool;
    // NUMA policy used to order domain preference during allocation.
    // nullptr means no topology information is available (treat all domains equally).
    const kernel::numa::NUMAPolicy* numaPolicy;
    // Precomputed per-CPU nearest pool table.  cpuNearestPool[p] is the DomainID
    // of the closest non-null NUMAPool for logical CPU p.  When numaPolicy is
    // non-null, ordered by the policy's domain fallback order.  When numaPolicy
    // is null (trivial single-domain topology), all entries point to the one pool.
    // Sentinel DomainID{} (UINT16_MAX) means no pool was found (should not occur
    // after createPageAllocator completes successfully).
    kernel::numa::DomainID cpuNearestPool[arch::MAX_PROCESSOR_COUNT];

    // Returns the nearest non-null NUMAPool for the given CPU, using the
    // precomputed cpuNearestPool table.  Asserts if no pool was found.
    [[nodiscard]] NUMAPool& nearestPool(arch::ProcessorID cpu) const {
        const kernel::numa::DomainID domain = cpuNearestPool[cpu];
        if (domain == kernel::numa::DomainID{}) {
            assert(unownedPool != nullptr, "no NUMAPool found for CPU");
            return *unownedPool;
        }
        return *numaPools[domain.value];
    }

    // Looks up the BigPageMetadata for the big page containing addr.
    // Returns nullptr if addr is outside all known ranges.
    BigPageMetadata* findMetadata(kernel::mm::phys_addr addr);
private:
    [[nodiscard]] inline size_t allocateFast(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags);
    [[nodiscard]] inline size_t allocateFallback(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags);
public:
    [[nodiscard]] size_t allocatePages(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags = {});
    void freePages(PageRef* pages, size_t count);

    // Reserve all small pages in range across all pools (init-time only).
    void reserveRange(kernel::mm::phys_memory_range range);

#ifdef CROCOS_TESTING
    // Sum of free subpages across all NUMAPools (and the unowned pool, if present).
    // Includes pages cached in LocalPools, since their SmallPageAllocators track occupancy
    // independently of which pool-level structure currently holds the BigPageMetadata.
    [[nodiscard]] size_t countFreePages() const;
    // True when the given PageRef is currently allocated (not available for new allocations).
    // For BIG refs: checks isFull() on the owning BigPageMetadata.
    // For SMALL refs: checks the occupancy bitmap in the owning SmallPageAllocator.
    [[nodiscard]] bool isPageAllocated(PageRef page);
#endif
};

NUMAPool*         createNumaPool(BootstrapAllocator& alloc,
                                 const Vector<kernel::mm::phys_memory_range>& ranges,
                                 kernel::numa::DomainID domain = kernel::numa::DomainID{0});
LocalPool*        createLocalPool(BootstrapAllocator& alloc,
                                  const kernel::numa::NUMATopology* topology = nullptr,
                                  NUMAPool* homePool = nullptr);

// CONTRACT: perDomainAllocs must be sized to (maxDomainID + 1), with nullptr
// slots for any domain IDs that have no physical memory.  This allows O(1)
// lookup via numaPools[domainID] in the hot allocation path.
// processorCount is the number of valid logical CPU IDs ([0, processorCount)).
// It is used to populate cpuNearestPool when numaPolicy is provided.
PageAllocatorImpl createPageAllocator(Vector<NUMAPool*>&& perDomainAllocs, LocalPool** localPools,
                                      size_t processorCount,
                                      NUMAPool* unownedPool = nullptr,
                                      const kernel::numa::NUMAPolicy* numaPolicy = nullptr);

// Global page allocator instance, set by initPageAllocator().
extern PageAllocatorImpl* gPageAllocator;

#endif //CROCOS_PAGEALLOCATOR_H