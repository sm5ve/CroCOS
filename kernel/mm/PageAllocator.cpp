//
// Created by Spencer Martin on 2/12/25.
//

#include <../include/mem/mm.h>
#include <arch.h>
#include "core/atomic.h"
#include <core/ds/Vector.h>
#include <kernel.h>
#include <core/math.h>

#define ALLOCATOR_DEBUG

namespace kernel::mm::PageAllocator{

    using BufferId = SmallestUInt_t<RequiredBits(arch::MAX_PROCESSOR_COUNT)>;
    const BufferId GLOBAL_POOL = arch::MAX_PROCESSOR_COUNT;

    struct ContiguousRangeAllocator{
    private:
        using SubpageIndexRawType = SmallestUInt_t<RequiredBits(smallPagesPerBigPage)>;
        using SuperpageIndexRawType = SmallestUInt_t<RequiredBits(bigPagesInMaxMemory)>;

        using SubpageStackMarker = SmallestUInt_t<RequiredBits(smallPagesPerBigPage + 1)>;
        using SuperpageStackMarker = uint64_t; //used for simplicity when doing atomic stuff.

        struct SubpagePool;
        struct RawSubpagePool{
        private:
            struct SubpageIndex{
                SubpageIndexRawType value;

                static SubpageIndex fromAddress(phys_addr addr){
                    return SubpageIndex((addr.value / smallPageSize) % smallPagesPerBigPage);
                }

                [[nodiscard]]
                uint64_t getOffsetIntoSuperpage() const{
                    return value * smallPageSize;
                }

                bool operator==(const SubpageIndex&) const = default;
            };
            struct SubpageFreeStackIndex{
                SubpageIndexRawType value;

                bool operator==(const SubpageFreeStackIndex&) const = default;
            };
            friend SubpagePool;
            SubpageIndex subpageFreeStack[smallPagesPerBigPage];
            SubpageFreeStackIndex subpageStackIndexMap[smallPagesPerBigPage];

            SubpageIndex& getSubpageIndex(SubpageIndex ind){
                return subpageFreeStack[subpageStackIndexMap[ind.value].value];
            }
            SubpageIndex& getSubpageIndex(SubpageFreeStackIndex ind){
                return subpageFreeStack[ind.value];
            }
            SubpageIndex& getSubpageIndex(SubpageStackMarker marker){
                return subpageFreeStack[marker];
            }

            SubpageFreeStackIndex& getSubpageFreeStackIndex(SubpageFreeStackIndex& ind){
                return ind;
            }

            SubpageFreeStackIndex& getSubpageFreeStackIndex(SubpageIndex ind){
                return subpageStackIndexMap[ind.value];
            }

            SubpageFreeStackIndex& getSubpageFreeStackIndex(SubpageStackMarker ind){
#ifdef ALLOCATOR_DEBUG
                assert(ind < smallPagesPerBigPage, "Tried to get out of bounds small page");
#endif
                return getSubpageFreeStackIndex(getSubpageIndex(ind));
            }

#ifdef ALLOCATOR_DEBUG
            template <typename T>
            void verifyMapSanity(T t){
                assert(getSubpageIndex(t) == getSubpageIndex(getSubpageFreeStackIndex(t)), "Subpage pool state insane");
                assert(getSubpageFreeStackIndex(t) == getSubpageFreeStackIndex(getSubpageIndex(t)), "Subpage pool state insane");
            }
#endif

            template <typename T, typename S>
            void swapPages(T t, S s){
#ifdef ALLOCATOR_DEBUG
                //This is a very paranoid check, but since the old allocator was having issues, I'd rather be paranoid.
                verifyMapSanity(t);
                verifyMapSanity(s);
#endif
                //It's important that we grab these references before doing the swaps, otherwise
                //getSubpageFreeStackIndex may yield unexpected results
                auto& sit = getSubpageIndex(t);
                auto& sis = getSubpageIndex(s);
                auto& sft = getSubpageFreeStackIndex(t);
                auto& sfs = getSubpageFreeStackIndex(s);
                swap(sit, sis);
                swap(sft, sfs);
#ifdef ALLOCATOR_DEBUG
                verifyMapSanity(t);
                verifyMapSanity(s);
#endif
            }

            void initialize(){
                for(size_t i = 0; i < smallPagesPerBigPage; i++){
                    subpageFreeStack[i] = SubpageIndex{(SubpageIndexRawType)i};
                    subpageStackIndexMap[i] = SubpageFreeStackIndex{(SubpageIndexRawType)i};
                }
            }
        };

        static_assert(sizeof(RawSubpagePool) == 2 * smallPagesPerBigPage * sizeof(SubpageIndexRawType), "RawSubpagePool of unexpected size");

        struct SubpagePool{
        private:
            RawSubpagePool& pool;
            SubpageStackMarker& bottomOfFreeMarker;
            phys_addr base;


            SubpageStackMarker topOfUsed(){
                return bottomOfFreeMarker - 1;
            }

        public:

            SubpagePool(RawSubpagePool& p, SubpageStackMarker& m, phys_addr b) : pool(p), bottomOfFreeMarker(m), base(b){
#ifdef ALLOCATOR_DEBUG
                assert(b.value % bigPageSize == 0, "Misaligned superpage");
#endif
            }

            [[nodiscard]]
            bool isFull() const{
#ifdef ALLOCATOR_DEBUG
                assert(bottomOfFreeMarker <= smallPagesPerBigPage, "Subpage stack bottomOfFreeMarker out of bounds");
#endif
                return bottomOfFreeMarker == smallPagesPerBigPage;
            }

            [[nodiscard]]
            bool isEmpty() const{
#ifdef ALLOCATOR_DEBUG
                assert(bottomOfFreeMarker <= smallPagesPerBigPage, "Subpage stack bottomOfFreeMarker out of bounds");
#endif
                return bottomOfFreeMarker == 0;
            }

            phys_addr allocateSubpage(){
#ifdef ALLOCATOR_DEBUG
                assert(!isFull(), "Tried to allocate small page from full pool");
#endif
                return phys_addr(pool.getSubpageIndex(bottomOfFreeMarker++).getOffsetIntoSuperpage() + base.value);
            }

            void freeSubpage(phys_addr addr){
                auto subpageIndex = RawSubpagePool::SubpageIndex::fromAddress(addr);
#ifdef ALLOCATOR_DEBUG
                assert(pool.getSubpageFreeStackIndex(subpageIndex).value < bottomOfFreeMarker, "Double-freed subpage");
#endif
                pool.swapPages(subpageIndex, topOfUsed());
                bottomOfFreeMarker--;
            }

            //Does not error if you reserve a page that is already allocated - this is to allow for simpler
            //initialization of the allocator. However, this will return 'false' if the page was already reserved,
            //so the caller may decide to error if it wishes
            bool reserveSubpage(phys_addr addr){
                auto subpageIndex = RawSubpagePool::SubpageIndex::fromAddress(addr);
                //If the subpage is already below the free marker, it's already allocated so we do nothing
                if(subpageIndex.value < bottomOfFreeMarker) {
                    return false;
                }
                //Otherwise, we swapPages the page to the bottom of the free zone and bump the marker
                pool.swapPages(subpageIndex, bottomOfFreeMarker);
                bottomOfFreeMarker++;
                return true;
            }

            void initialize(){
                bottomOfFreeMarker = 0;
                pool.initialize();
            }
        };

        struct SuperpagePool{
        public:
            struct SuperpageIndex{
                SuperpageIndexRawType value;

                static SuperpageIndex fromAddress(phys_addr addr, phys_addr b){
                    return SuperpageIndex((SuperpageIndexRawType)((addr.value - b.value)/ bigPageSize));
                }

                [[nodiscard]]
                phys_addr toAddress(phys_addr b) const{
                    return phys_addr(value * bigPageSize + b.value);
                }

                bool operator==(const SuperpageIndex&) const = default;
            };
            struct SuperpageFreeStackIndex{
                SuperpageIndexRawType value;
                BufferId bufferId;

                bool operator==(const SuperpageFreeStackIndex&) const = default;
            };

        private:
            SuperpageIndex* superpagePool;
            SuperpageFreeStackIndex* superpagePoolIndexMap;
            SuperpageStackMarker poolSize;
            const uint64_t maxPoolSize;
            const BufferId bufferId;

            SuperpageIndex& getSuperpageIndex(SuperpageFreeStackIndex fsi){
#ifdef ALLOCATOR_DEBUG
                assert(fsi.bufferId == bufferId, "Tried to get superpage from wrong pool");
#endif
                return superpagePool[fsi.value];
            }
            SuperpageIndex& getSuperpageIndex(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(addr.value % bigPageSize == 0, "Misaligned superpage");
#endif
                return getSuperpageIndex(SuperpageIndex::fromAddress(addr, base));
            }
            SuperpageIndex& getSuperpageIndex(SuperpageIndex spi){
                auto poolIndex = superpagePoolIndexMap[spi.value];
                return getSuperpageIndex(poolIndex);
            }
            SuperpageIndex& getSuperpageIndex(SuperpageStackMarker ssm){
#ifdef ALLOCATOR_DEBUG
                assert(ssm < poolSize, "Tried to get out of bounds superpage");
#endif
                return superpagePool[ssm];
            }

            SuperpageFreeStackIndex& getFreeStackIndex(SuperpageIndex spi){
                auto& out = superpagePoolIndexMap[spi.value];
#ifdef ALLOCATOR_DEBUG
                assert(out.bufferId == bufferId, "Tried to get superpage from wrong pool");
#endif
                return out;
            }
            SuperpageFreeStackIndex& getFreeStackIndex(SuperpageFreeStackIndex& fsi){
                return fsi;
            }
            SuperpageFreeStackIndex& getFreeStackIndex(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(addr.value % bigPageSize == 0, "Misaligned superpage");
#endif
                return getFreeStackIndex(SuperpageIndex::fromAddress(addr, base));
            }


            SuperpageStackMarker poolTopMarker(){
#ifdef ALLOCATOR_DEBUG
                assert(poolSize > 0, "Tried to get top of empty pool");
#endif
                return poolSize - 1;
            }

            SuperpageIndex& poolTop(){
                return getSuperpageIndex(poolTopMarker());
            }

            void transferSuperpageOwner(SuperpageIndex ind, BufferId priorOwner, SuperpageIndexRawType newPosition){
                SuperpageFreeStackIndex& fsi = superpagePoolIndexMap[ind.value];
#ifdef ALLOCATOR_DEBUG
                assert(fsi.bufferId == priorOwner, "Tried to transfer superpage from different owner than expected");
#else
                (void)priorOwner;
#endif
                fsi.bufferId = bufferId;
                fsi.value = newPosition;
            }
        public:
#ifdef ALLOCATOR_DEBUG
            template <typename T>
            void verifyMapSanity(T t){
                assert(getSuperpageIndex(t) == getSuperpageIndex(getFreeStackIndex(t)), "Superpage pool state insane 1");
                assert(getFreeStackIndex(t) == getFreeStackIndex(getSuperpageIndex(t)), "Superpage pool state insane 2");
                assert(getFreeStackIndex(t).bufferId == bufferId, "Superpage pool state insane 3");
            }
#endif
            RWSpinlock lock;
            const phys_addr base;

            SuperpagePool(void* spp, void* spim, phys_addr b, SuperpageStackMarker initSize, uint64_t maxSize, BufferId bid) :
            maxPoolSize(maxSize), bufferId(bid), base(b){
                superpagePool = (SuperpageIndex*) spp;
                superpagePoolIndexMap = (SuperpageFreeStackIndex*) spim;
                poolSize = initSize;
                assert(b.value % bigPageSize == 0, "misaligned superpage pool base");
                //If we're the global pool, we're in charge of initializing all our data
                if(bid == GLOBAL_POOL){
                    assert(initSize == maxSize, "Global pool should have all superpages on initialization");
                    for(size_t i = 0; i < maxSize; i++){
                        superpagePool[i] = SuperpageIndex((SuperpageIndexRawType) i);
                        superpagePoolIndexMap[i] = SuperpageFreeStackIndex((SuperpageIndexRawType) i, GLOBAL_POOL);
                    }
                }
            }

            template <typename T, typename S>
            void swapPages(T t, S s){
#ifdef ALLOCATOR_DEBUG
                //This is a very paranoid check, but since the old allocator was having issues, I'd rather be paranoid.
                verifyMapSanity(t);
                verifyMapSanity(s);
                assert(lock.writer_lock_taken(), "It is unsafe to call this method without acquiring the writer acquire on the pool");
#endif
                //It's important that we grab these references before doing the swaps, otherwise
                //getFreeStackIndex may yield unexpected results
                auto& spt = getSuperpageIndex(t);
                auto& sps = getSuperpageIndex(s);
                auto& sft = getFreeStackIndex(t);
                auto& sfs = getFreeStackIndex(s);

                swap(spt, sps);
                swap(sft, sfs);
#ifdef ALLOCATOR_DEBUG
                verifyMapSanity(t);
                verifyMapSanity(s);
#endif
            }

            template <typename T, typename S, typename R>
            void rotatePagesLeft(T t, S s, R r){
#ifdef ALLOCATOR_DEBUG
                //This is a very paranoid check, but since the old allocator was having issues, I'd rather be paranoid.
                verifyMapSanity(t);
                verifyMapSanity(s);
                verifyMapSanity(r);
                assert(lock.writer_lock_taken(), "It is unsafe to call this method without acquiring the writer acquire on the pool");
#endif
                //It's important that we grab these references before doing the swaps, otherwise
                //getFreeStackIndex may yield unexpected results
                auto& spt = getSuperpageIndex(t);
                auto& sps = getSuperpageIndex(s);
                auto& spr = getSuperpageIndex(r);
                auto& sft = getFreeStackIndex(t);
                auto& sfs = getFreeStackIndex(s);
                auto& sfr = getFreeStackIndex(r);
#ifdef ALLOCATOR_DEBUG
                assert(spt != sps, "Arguments to rotate must point to distinct pages");
                assert(spt != spr, "Arguments to rotate must point to distinct pages");
                assert(spr != sps, "Arguments to rotate must point to distinct pages");
                assert(sft != sfs, "Arguments to rotate must point to distinct pages");
                assert(sft != sfr, "Arguments to rotate must point to distinct pages");
                assert(sfr != sfs, "Arguments to rotate must point to distinct pages");
#endif
                rotateLeft(spt, sps, spr);
                //You need to do the inverse permutation for the inverse mapping, you dummy!
                rotateRight(sft, sfs, sfr);

#ifdef ALLOCATOR_DEBUG
                verifyMapSanity(t); //asserts here
                verifyMapSanity(s);
                verifyMapSanity(r);
#endif
            }

            template <typename T, typename S, typename R>
            void rotatePagesRight(T t, S s, R r){
#ifdef ALLOCATOR_DEBUG
                //This is a very paranoid check, but since the old allocator was having issues, I'd rather be paranoid.
                verifyMapSanity(t);
                verifyMapSanity(s);
                verifyMapSanity(r);
                assert(lock.writer_lock_taken(), "It is unsafe to call this method without acquiring the writer acquire on the pool");
#endif
                //It's important that we grab these references before doing the swaps, otherwise
                //getFreeStackIndex may yield unexpected results
                auto& spt = getSuperpageIndex(t);
                auto& sps = getSuperpageIndex(s);
                auto& spr = getSuperpageIndex(r);
                auto& sft = getFreeStackIndex(t);
                auto& sfs = getFreeStackIndex(s);
                auto& sfr = getFreeStackIndex(r);
#ifdef ALLOCATOR_DEBUG
                assert(spt != sps, "Arguments to rotate must point to distinct pages");
                assert(spt != spr, "Arguments to rotate must point to distinct pages");
                assert(spr != sps, "Arguments to rotate must point to distinct pages");
                assert(sft != sfs, "Arguments to rotate must point to distinct pages");
                assert(sft != sfr, "Arguments to rotate must point to distinct pages");
                assert(sfr != sfs, "Arguments to rotate must point to distinct pages");
#endif
                rotateRight(spt, sps, spr);
                rotateLeft(sft, sfs, sfr);
#ifdef ALLOCATOR_DEBUG
                verifyMapSanity(t);
                verifyMapSanity(s);
                verifyMapSanity(r);
#endif
            }

            bool isEmpty(){
                return poolSize == 0;
            }

            //To be used for taking a high number of superpages. The caller is required to acquire the writer acquire on
            //both this pool and the argument pool
            void takePageFromExclusive(SuperpagePool& pool){
#ifdef ALLOCATOR_DEBUG
                assert(!pool.isEmpty(), "Tried to steal page from empty pool");
                assert(pool.lock.writer_lock_taken(),
                       "It is unsafe to call this method without acquiring the writer acquire on the source pool");
                assert(lock.writer_lock_taken(),
                       "It is unsafe to call this method without acquiring the writer acquire on the target pool");
#endif
                auto newPage = pool.poolTop();
                pool.poolSize--;
                transferSuperpageOwner(newPage, pool.bufferId, (SuperpageIndexRawType) poolSize);
                superpagePool[poolSize++] = newPage;
#ifdef ALLOCATOR_DEBUG
                assert(poolSize <= maxPoolSize, "Pool has somehow grown too large");
                verifyMapSanity(newPage);
#endif
            }

            //To be used for taking a small number of pages with the possibility of doing so concurrently.
            //Useful in grabbing pages from the global pool
            bool tryStealPage(SuperpagePool& pool){
#ifdef ALLOCATOR_DEBUG
                assert(lock.writer_lock_taken(),
                       "It is unsafe to call this method without acquiring the writer acquire on the target pool");
#endif
                pool.lock.acquire_reader();
                SuperpageIndex newPage;
                while(true){
                    auto oldSize = pool.poolSize;
                    if(oldSize == 0){
                        pool.lock.release_reader();
                        return false;
                    }
                    newPage = pool.poolTop();
                    if(atomic_cmpxchg(pool.poolSize, oldSize, oldSize - 1)){
                        pool.lock.release_reader();
                        break;
                    }
                }
                transferSuperpageOwner(newPage, pool.bufferId, (SuperpageIndexRawType) poolSize);
                superpagePool[poolSize++] = newPage;
#ifdef ALLOCATOR_DEBUG
                assert(poolSize <= maxPoolSize, "Pool has somehow grown too large");
                verifyMapSanity(newPage);
#endif
                return true;
            }

            template <typename T>
            bool isBelowMarker(T t, SuperpageStackMarker marker){
                return getFreeStackIndex(t).value < marker;
            }

            template <typename T>
            bool isAtOrAboveMarker(T t, SuperpageStackMarker marker){
                return getFreeStackIndex(t).value >= marker;
            }

            SuperpageIndex fromAddress(phys_addr addr){
                return SuperpageIndex::fromAddress(addr, base);
            }

            SuperpageIndex fromMarker(SuperpageStackMarker marker){
                return this -> getSuperpageIndex(marker);
            }

            size_t getPoolSize(){
                return poolSize;
            }

            void movePageToTop(phys_addr addr){
                swapPages(fromAddress(addr), poolTop());
            }
        };

        struct alignas(arch::CACHE_LINE_SIZE) LocalPool{
        private:
            SuperpagePool& spp;
            SuperpageStackMarker fullyOccupiedZoneStart;
            SuperpageStackMarker freeZoneStart;

            SuperpageStackMarker partiallyOccupiedZoneTop(){
#ifdef ALLOCATOR_DEBUG
                assert(fullyOccupiedZoneStart != 0, "partially occupied zone is empty");
#endif
                return fullyOccupiedZoneStart - 1;
            }

            SuperpageStackMarker fullyOccupiedZoneTop(){
                return freeZoneStart - 1;
            }

            void movePageFromFreeToFull(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, freeZoneStart), "Tried to move page that isn't free");
#endif
                spp.swapPages(spp.fromAddress(addr), spp.fromMarker(freeZoneStart));
                freeZoneStart++;
#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, fullyOccupiedZoneStart), "movePageFromFreeToFull failed");
                assert(spp.isBelowMarker(addr, freeZoneStart), "movePageFromFreeToFull failed");
#endif
            }

            void movePageFromFullToFree(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, freeZoneStart), "Tried to move page that isn't full");
                assert(spp.isAtOrAboveMarker(addr, fullyOccupiedZoneStart), "Tried to move page that isn't full");
#endif
                spp.swapPages(spp.fromAddress(addr), spp.fromMarker(fullyOccupiedZoneTop()));
                freeZoneStart--;
#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, freeZoneStart), "movePageFromFullToFree failed");
#endif
            }

            void movePageFromFullToPartiallyOccupied(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, freeZoneStart), "Tried to move page that isn't full");
                assert(spp.isAtOrAboveMarker(addr, fullyOccupiedZoneStart), "Tried to move page that isn't full");
#endif
                spp.swapPages(spp.fromAddress(addr), spp.fromMarker(fullyOccupiedZoneStart));
                fullyOccupiedZoneStart++;
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, fullyOccupiedZoneStart), "movePageFromFullToPartiallyOccupied failed");
#endif
            }

            void movePageFromPartiallyOccupiedToFull(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, fullyOccupiedZoneStart), "Tried to move page that isn't partially occupied");
#endif
                spp.swapPages(spp.fromAddress(addr), spp.fromMarker(partiallyOccupiedZoneTop()));
                fullyOccupiedZoneStart--;
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, freeZoneStart), "movePageFromPartiallyOccupiedToFull failed");
                assert(spp.isAtOrAboveMarker(addr, fullyOccupiedZoneStart), "movePageFromPartiallyOccupiedToFull failed");
#endif
            }

            void movePageFromFreeToPartiallyOccupied(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, freeZoneStart), "Tried to move page that isn't free");
                spp.verifyMapSanity(spp.fromAddress(addr));
#endif
                auto addrSuperpageIndex = spp.fromAddress(addr);
                auto freeZoneStartSuperpageIndex = spp.fromMarker(freeZoneStart);
                auto fullyOccupiedZoneStartSuperpageIndex = spp.fromMarker(fullyOccupiedZoneStart);
                //If the fully occupied zone is empty, do a swap
                if (freeZoneStartSuperpageIndex == fullyOccupiedZoneStartSuperpageIndex) {
                    spp.swapPages(spp.fromAddress(addr), freeZoneStartSuperpageIndex);
                }
                //If we're freeing the base page of the free zone, also swap
                else if (addrSuperpageIndex == freeZoneStartSuperpageIndex) {
                    spp.swapPages(spp.fromAddress(addr), fullyOccupiedZoneStartSuperpageIndex);
                }
                //Otherwise rotate
                else {
                    spp.rotatePagesRight(addrSuperpageIndex, fullyOccupiedZoneStartSuperpageIndex, freeZoneStartSuperpageIndex);
                }
                freeZoneStart++;
                fullyOccupiedZoneStart++;
#ifdef ALLOCATOR_DEBUG
                spp.verifyMapSanity(spp.fromAddress(addr));
                assert(spp.isBelowMarker(addr, fullyOccupiedZoneStart), "movePageFromFreeToPartiallyOccupied failed");
#endif
            }

            void movePageFromPartiallyOccupiedToFree(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(addr, fullyOccupiedZoneStart), "Tried to move page that isn't partially occupied");
#endif
                auto addrSuperpageIndex = spp.fromAddress(addr);
                auto partiallyOccupiedZoneTopSuperpageIndex = spp.fromMarker(partiallyOccupiedZoneTop());
                auto fullyOccupiedZoneTopSuperpageIndex = spp.fromMarker(fullyOccupiedZoneTop());
                //If the fully occupied zone is empty, swap the partially occupied page up to be the new base
                //of the free zone
                if (partiallyOccupiedZoneTopSuperpageIndex == fullyOccupiedZoneTopSuperpageIndex) {
                    spp.swapPages(addrSuperpageIndex, fullyOccupiedZoneTopSuperpageIndex);
                }
                //If we're at the top of the partially occupied pool, we can also swap.
                else if (addrSuperpageIndex == partiallyOccupiedZoneTopSuperpageIndex) {
                    spp.swapPages(addrSuperpageIndex, fullyOccupiedZoneTopSuperpageIndex);
                }
                else {
                    spp.rotatePagesLeft(spp.fromAddress(addr), spp.fromMarker(partiallyOccupiedZoneTop()),
                                        spp.fromMarker(fullyOccupiedZoneTop()));
                }
                freeZoneStart--;
                fullyOccupiedZoneStart--;

#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, freeZoneStart), "movePageFromPartiallyOccupiedToFree failed");
#endif
            }

            void moveFreePageToTopOfPartiallyAllocated(){
                spp.swapPages(spp.fromMarker(fullyOccupiedZoneStart), spp.fromMarker(freeZoneStart));
                fullyOccupiedZoneStart++;
                freeZoneStart++;
            }
        public:
            LocalPool(SuperpagePool& pp) : spp(pp){
                fullyOccupiedZoneStart = 0;
                freeZoneStart = 0;
            }

            void acquireLock(){
                spp.lock.acquire_writer();
            }

            void releaseLock(){
                spp.lock.release_writer();
            }

            size_t remainingFreeSuperpages(){
                return spp.getPoolSize() - freeZoneStart;
            }

            size_t remainingPartiallyAllocatedSuperpages(){
                return fullyOccupiedZoneStart;
            }

            bool hasFreeSuperpages(){
                return remainingFreeSuperpages() != 0;
            }

            bool hasPartiallyAllocatedSuperpages(){
                return remainingPartiallyAllocatedSuperpages() != 0;
            }

            phys_addr allocateSuperpage(){
#ifdef ALLOCATOR_DEBUG
                assert(hasFreeSuperpages(), "Tried to allocate superpage when pool has none free");
#endif
                return spp.fromMarker(freeZoneStart++).toAddress(spp.base);
            }

            void reserveSuperpage(phys_addr addr){
                auto page = spp.fromAddress(addr);
                //if the page we want to reserve is currently partially occupied call the appropriate method
                if(spp.isBelowMarker(page, fullyOccupiedZoneStart)){
                    movePageFromPartiallyOccupiedToFull(addr);
                }
                    //otherwise if it's free, call the corresponding method
                else if(spp.isAtOrAboveMarker(page, freeZoneStart)){
                    movePageFromFreeToFull(addr);
                }

#ifdef ALLOCATOR_DEBUG
                assert(spp.isAtOrAboveMarker(addr, fullyOccupiedZoneStart) && spp.isBelowMarker(addr, freeZoneStart), "Superpage not in fully occupied zone???");
#endif
            }

            void reserveSuperpageAsPartiallyAllocated(phys_addr addr){
                auto page = spp.fromAddress(addr);
                //if the page we want to reserve is currently partially occupied call the appropriate method
                if(spp.isAtOrAboveMarker(page, freeZoneStart)){
                    movePageFromFreeToPartiallyOccupied(addr);
                }
            }

            void markFullSuperpageFree(phys_addr addr){
                movePageFromFreeToFull(addr);
            }

            phys_addr getPageForSubpageAllocation(){
                if(!hasPartiallyAllocatedSuperpages()){
#ifdef ALLOCATOR_DEBUG
                    assert(hasFreeSuperpages(), "LocalPool completely full");
#endif
                    moveFreePageToTopOfPartiallyAllocated();
                }
#ifdef ALLOCATOR_DEBUG
                assert(hasPartiallyAllocatedSuperpages(), "LocalPool state insane");
#endif
                return spp.fromMarker(partiallyOccupiedZoneTop()).toAddress(spp.base);
            }

            void markTopPartiallyAllocatedPageAsFull(){
#ifdef ALLOCATOR_DEBUG
                assert(hasPartiallyAllocatedSuperpages(), "Tried to mark nonexistent partially allocate page as full");
#endif
                fullyOccupiedZoneStart--;
            }

            void markPartiallyAllocatedPageAsFull(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
                assert(spp.isBelowMarker(spp.fromAddress(addr), fullyOccupiedZoneStart), "Page not partially occupied");
#endif
                spp.swapPages(spp.fromAddress(addr), spp.fromMarker(partiallyOccupiedZoneTop()));
                markTopPartiallyAllocatedPageAsFull();
            }

            void ensureSuperpageMarkedPartiallyOccupied(phys_addr addr){
                auto page = spp.fromAddress(addr);
                if(spp.isAtOrAboveMarker(page, freeZoneStart)){
                    movePageFromFreeToPartiallyOccupied(addr);
                }
                //If it's already marked as partially occupied, bail
                else if(spp.isBelowMarker(page, fullyOccupiedZoneStart)){
                    return;
                }
                else{
                    movePageFromFullToPartiallyOccupied(addr);
                }
            }

            void markPartiallyOccupiedSuperpageFree(phys_addr addr){
                movePageFromPartiallyOccupiedToFree(addr);
            }

            bool stealPageFrom(SuperpagePool& otherPool){
                return spp.tryStealPage(otherPool);
            }
        };

        struct LocalAllocator{
        private:
            LocalPool& localPool;
            SuperpagePool& globalPool;
            RawSubpagePool* subpagePools;
            SubpageStackMarker* subpageStackMarkers;

            auto subpagePoolForSuperpage(phys_addr addr){
                addr = phys_addr(addr.value & ~(bigPageSize - 1));
                size_t index = (addr.value - globalPool.base.value) / bigPageSize;
                return SubpagePool(subpagePools[index], subpageStackMarkers[index], addr);
            }
        public:
            LocalAllocator(LocalPool& lp, SuperpagePool& gp, RawSubpagePool* sp, SubpageStackMarker* ssm) :
            localPool(lp), globalPool(gp), subpagePools(sp), subpageStackMarkers(ssm){}

            phys_addr allocateBigPage(){
                //Make sure the
                localPool.acquireLock();
                if(!localPool.hasFreeSuperpages()){
                    bool didSteal = localPool.stealPageFrom(globalPool);
                    assert(didSteal, "Global pool out of memory");
                }
                auto out = localPool.allocateSuperpage();
                localPool.releaseLock();
                return out;
            }

            //TODO keep track of the number of small pages allocated to help determine the bulk allocation strategy
            //when the time comes
            phys_addr allocateSmallPage(){
                localPool.acquireLock();
                if(!(localPool.hasFreeSuperpages() || localPool.hasPartiallyAllocatedSuperpages())){
                    auto didSteal = localPool.stealPageFrom(globalPool);
                    assert(didSteal, "Global pool out of memory");
                }
                phys_addr superPage = localPool.getPageForSubpageAllocation();
                localPool.releaseLock();
                auto out = subpagePoolForSuperpage(superPage).allocateSubpage();
                if(subpagePoolForSuperpage(superPage).isFull()){
                    localPool.markTopPartiallyAllocatedPageAsFull();
                }
                return out;
            }

            void freeBigPage(phys_addr addr){
                localPool.acquireLock();
                localPool.markFullSuperpageFree(addr);
                localPool.releaseLock();
            }

            void freeSmallPage(phys_addr addr){
                localPool.acquireLock();
                auto smallPool = subpagePoolForSuperpage(addr);
                smallPool.freeSubpage(addr);
                phys_addr aligned_base(addr.value & ~(bigPageSize - 1));
                if(smallPool.isEmpty()){
                    localPool.markPartiallyOccupiedSuperpageFree(aligned_base);
                }
                else{
                    localPool.ensureSuperpageMarkedPartiallyOccupied(aligned_base);
                }
                localPool.releaseLock();
            }

            void reserveSmallPage(phys_addr addr){
                phys_addr aligned_base(addr.value & ~(bigPageSize - 1));
                localPool.acquireLock();
                localPool.ensureSuperpageMarkedPartiallyOccupied(aligned_base);
                auto pool = subpagePoolForSuperpage(addr);
                pool.reserveSubpage(addr);

                if(pool.isFull()){
                    localPool.markPartiallyAllocatedPageAsFull(addr);
                }
                localPool.releaseLock();
            }

            void reserveBigPage(phys_addr addr){
                phys_addr aligned_base(addr.value & ~(bigPageSize - 1));
                localPool.acquireLock();
                localPool.reserveSuperpage(aligned_base);
                localPool.releaseLock();
            }
        };

        RawSubpagePool* subpagePools;
        SubpageStackMarker* subpageFreeMarkers;
        SuperpagePool::SuperpageFreeStackIndex* superpageFreeIndices;
        SuperpagePool* superpagePools;
        SuperpagePool* globalPool;
        LocalPool* localPools;
        LocalAllocator* localAllocators;
        phys_memory_range range;

        static void incrementPtrCacheAligned(void*& ptr, size_t amt){
            assert((uint64_t)ptr % arch::CACHE_LINE_SIZE == 0, "buffer not cache line aligned");
            amt = divideAndRoundUp(amt, arch::CACHE_LINE_SIZE) * arch::CACHE_LINE_SIZE;
            ptr = (void*)((size_t)ptr + amt);
        }

        [[nodiscard]]
        BufferId getBufferIDForAddress(phys_addr addr){
#ifdef ALLOCATOR_DEBUG
            assert(range.contains(addr), "address out of range for allocator");
#endif
            auto aligned = (addr.value & ~(bigPageSize - 1));
            auto index = (aligned - range.start.value) / bigPageSize;
            return superpageFreeIndices[index].bufferId;
        }

        void moveSpecificBigPageFromGlobalPoolToLocalPool(phys_addr addr, BufferId bid){
            phys_addr aligned(addr.value & ~(bigPageSize - 1));
            assert(getBufferIDForAddress(addr) == GLOBAL_POOL, "Tried to move big page that was not in global pool");
            assert(bid != GLOBAL_POOL, "Tried to move big page to global pool");

            localPools[bid].acquireLock();
            globalPool -> lock.acquire_writer();
            globalPool->movePageToTop(addr);
            globalPool -> lock.release_writer();
            localPools[bid].stealPageFrom(*globalPool);
            localPools[bid].releaseLock();
        }

        void reserveSmallPage(phys_addr addr){
            BufferId bid = getBufferIDForAddress(addr);
            if(bid == GLOBAL_POOL){
                moveSpecificBigPageFromGlobalPoolToLocalPool(addr, 0);
                bid = 0;
            }
            auto allocator = localAllocators[bid];
            allocator.reserveSmallPage(addr);
        }

        void reserveBigPage(phys_addr addr){
            BufferId bid = getBufferIDForAddress(addr);
            if(bid == GLOBAL_POOL){
                moveSpecificBigPageFromGlobalPoolToLocalPool(addr, 0);
                bid = 0;
            }
            auto allocator = localAllocators[bid];
            allocator.reserveBigPage(addr);
        }

        void reserveOverlap(phys_memory_range trueRange){
            phys_addr bigPageAlignedTop = phys_addr(roundUpToNearestMultiple(trueRange.end.value, bigPageSize));
            phys_addr bigPageAlignedBot = phys_addr(roundDownToNearestMultiple(trueRange.start.value, bigPageSize));

            reservePhysMemoryRange({bigPageAlignedBot, trueRange.start}); //Reserve stuff below the start of the memory range
            reservePhysMemoryRange({trueRange.end, bigPageAlignedTop}); //Reserve stuff above the end of the memory range
        }

    public:
        void reservePhysMemoryRange(phys_memory_range to_reserve){
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
                    reserveBigPage(toReserve);
                    toReserve.value += bigPageSize;
                }
                else{
                    reserveSmallPage(toReserve);
                    toReserve.value += smallPageSize;
                }
            }
        }

        ContiguousRangeAllocator(page_allocator_range_info info, size_t processorCount){
            phys_addr rangeBottom(divideAndRoundDown(info.range.start.value, bigPageSize) * bigPageSize);
            phys_addr rangeTop(divideAndRoundUp(info.range.end.value, bigPageSize) * bigPageSize);
            size_t totalSuperpages = (rangeTop.value - rangeBottom.value) / bigPageSize;
            void* buffPtr = info.buffer_start;
            assert((uint64_t)buffPtr % smallPageSize == 0, "Buffer not page-aligned");
            subpagePools = (RawSubpagePool*)buffPtr;
            incrementPtrCacheAligned(buffPtr, sizeof(RawSubpagePool) * totalSuperpages);
            subpageFreeMarkers = (SubpageStackMarker*)buffPtr;
            incrementPtrCacheAligned(buffPtr, sizeof(SubpageStackMarker) * totalSuperpages);
            superpagePools = (SuperpagePool*) kmalloc(sizeof(SuperpagePool) * (processorCount + 1), std::align_val_t{arch::CACHE_LINE_SIZE});
            localPools = (LocalPool*) kmalloc(sizeof(LocalPool) * processorCount, std::align_val_t{arch::CACHE_LINE_SIZE});
            localAllocators = (LocalAllocator*) kmalloc(sizeof(LocalAllocator) * processorCount, std::align_val_t{arch::CACHE_LINE_SIZE});

            superpageFreeIndices = (SuperpagePool::SuperpageFreeStackIndex*) buffPtr;
            incrementPtrCacheAligned(buffPtr, sizeof(SuperpagePool::SuperpageFreeStackIndex) * totalSuperpages);

            globalPool = &superpagePools[processorCount];

            for(size_t i = 0; i < processorCount + 1; i++){
                void* spp = buffPtr;
                incrementPtrCacheAligned(buffPtr, sizeof(SuperpagePool::SuperpageIndex) * totalSuperpages);
                BufferId bid = (BufferId)i;
                size_t initSize = 0;
                if(i == processorCount){
                    bid = GLOBAL_POOL;
                    initSize = totalSuperpages;
                }
                new (&superpagePools[i]) SuperpagePool(spp, superpageFreeIndices, rangeBottom, initSize, totalSuperpages, bid);
                if(i < processorCount){
                    new(&localPools[i]) LocalPool(superpagePools[i]);
                    new(&localAllocators[i]) LocalAllocator(localPools[i], *globalPool, subpagePools, subpageFreeMarkers);
                }
            }

            //Initialize the subpage pools
            for(size_t i = 0; i < totalSuperpages; i++){
                SubpagePool(subpagePools[i], subpageFreeMarkers[i], phys_addr(rangeBottom.value + i * bigPageSize)).initialize();
            }

            range = phys_memory_range(rangeBottom, rangeTop);

            reserveOverlap(info.range);
        }

        phys_addr allocateSmallPage(){
            auto allocator = localAllocators[arch::getCurrentProcessorID()];
            return allocator.allocateSmallPage();
        }

        void freeSmallPage(phys_addr addr){
            auto id = getBufferIDForAddress(addr);
            assert(id != GLOBAL_POOL, "tried to free address owned by global pool");
            localAllocators[id].freeSmallPage(addr);
        }

        static const size_t rawSubpagePoolSize = sizeof(RawSubpagePool);
        static const size_t subpageStackMarkerSize = sizeof(SubpageStackMarker);
        static const size_t superpageIndexMarkerSize = sizeof(SuperpagePool::SuperpageIndex);
        static const size_t superpageFreeStackIndex = sizeof(SuperpagePool::SuperpageFreeStackIndex);
    };

    WITH_GLOBAL_CONSTRUCTOR(Vector<ContiguousRangeAllocator>, allocators);

    void init(Vector<page_allocator_range_info>& regions, size_t processor_count){
        for(auto region : regions){
            allocators.push(ContiguousRangeAllocator(region, processor_count));
        }
    }

    void incrementSizeWithAlignment(size_t& size, size_t amount){
        size += divideAndRoundUp(amount, arch::CACHE_LINE_SIZE) * arch::CACHE_LINE_SIZE;
    }

    size_t requestedBufferSizeForRange(mm::phys_memory_range range, size_t processor_count){
        size_t out = 0;
        phys_addr rangeBottom(divideAndRoundDown(range.start.value, bigPageSize) * bigPageSize);
        phys_addr rangeTop(divideAndRoundUp(range.end.value, bigPageSize) * bigPageSize);
        size_t totalSuperpages = (rangeTop.value - rangeBottom.value) / bigPageSize;

        incrementSizeWithAlignment(out, ContiguousRangeAllocator::rawSubpagePoolSize * totalSuperpages);
        incrementSizeWithAlignment(out, ContiguousRangeAllocator::subpageStackMarkerSize * totalSuperpages);

        incrementSizeWithAlignment(out, ContiguousRangeAllocator::superpageFreeStackIndex * totalSuperpages);

        for(size_t i = 0; i < processor_count + 1; i++){
            incrementSizeWithAlignment(out, ContiguousRangeAllocator::superpageIndexMarkerSize * totalSuperpages);
        }

        return out;
    }

    void reservePhysicalRange(phys_memory_range range){
        //temporary just to confirm things are working okay
        //We will have to replace this by an iteration over the vector in time.
        allocators[0].reservePhysMemoryRange(range);
    }

    phys_addr allocateSmallPage(){
        auto out = allocators[0].allocateSmallPage();
        return out;
    }

    void freeSmallPage(phys_addr addr){
        allocators[0].freeSmallPage(addr);
    }
}