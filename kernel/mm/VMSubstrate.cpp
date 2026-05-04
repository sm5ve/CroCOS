//
// Created by Spencer Martin on 4/26/26.
//

#include <mem/VMSubstrate.h>
#include <mem/mm.h>
#include <arch.h>

#include <kmemlayout.h>

#include "arch.h"
#include "mem/TempWindow.h"

namespace VMSubstrateHelper{

    // ── Leaf page table wrapper ──────────────────────────────────────────────
    //
    // Wraps the bottom-level page table (arch::PageTable<leafLevel>) with two
    // 512-bit occupancy bitmaps embedded in the topmost 32 reserved entries:
    //
    //   PTEs 0–(N-1)  leaf entries mapping per-CPU dirty-bitmap pages  (N = dirtyWordCount())
    //   PTEs 480–495  allocBitmap words 0–15  (plain uint32_t, allocator-only)
    //   PTEs 496–511  freeBitmap words 0–15   (Atomic<uint32_t>, any freeing CPU)
    //   PTE  511      also holds freeWordCount in its uint16_t field
    //
    // Each dirty-bitmap page (4 KiB) holds one Atomic<uint64_t> per leaf-PT entry (8 bytes × 512).
    // The dirty word for entry k and CPU word dw is at virtual address:
    //     tableBase + dw * smallPageSize + k * sizeof(uint64_t)
    // where tableBase is the 2 MiB-aligned virtual base of the range this leaf PT covers.
    // Dirty pages are installed by the caller immediately after construction.
    //
    // Word i covers PTE indices [i*32, i*32+32).  Words 1–14 (covering PTEs 32–479)
    // start fully set; word 0 starts with bits [N, 32) set (bits 0..N-1 cleared by the
    // dirty-bitmap reservation); word 15 (covering the 32 reserved PTEs) is permanently 0.
    //
    // freeWordCount ∈ [0, 32]: counts words with ≥1 free bit across both bitmaps.
    // It changes only at two sites:
    //   • allocator, when allocBitmap word goes nonzero → 0: fetch_sub
    //   • freeing CPU, when freeBitmap word goes 0 → nonzero: fetch_add
    // The exchange-and-drain step (freeBitmap[w] → allocBitmap[w]) never touches
    // freeWordCount because the "credit" moves between the two bitmaps atomically
    // from the counter's perspective.

    constexpr size_t leafLevel      = arch::pageTableDescriptor.LEVEL_COUNT - 1;
    constexpr size_t wordWidth      = 32;
    constexpr size_t kEntryCount    = arch::pageTableDescriptor.entryCount[leafLevel]; // 512
    constexpr size_t kBitmapWords   = kEntryCount / wordWidth;               // 512 bits / 32 per word
    constexpr size_t kUsableEntries = kEntryCount - 2 * kBitmapWords; // 480
    constexpr size_t kAllocStart    = kUsableEntries;                 // first allocBitmap PTE (480)
    constexpr size_t kFreeStart     = kUsableEntries + kBitmapWords;  // first freeBitmap PTE (496)

    using LeafPTE  = arch::PTE<leafLevel>;
    using LeafMeta = arch::PTEMetadataEntry<arch::pageTableDescriptor.levels[leafLevel]>;

    struct LeafPageTableWrapper {
        arch::PageTable<leafLevel> table;

        // ── Internal accessors ───────────────────────────────────────────────

        LeafMeta& metaAt(size_t index) {
            return *reinterpret_cast<LeafMeta*>(&table[index]);
        }

        uint32_t& allocWord(size_t i) {
            return metaAt(kAllocStart + i).u32();
        }
        Atomic<uint32_t>& freeWord(size_t i) {
            return metaAt(kFreeStart + i).atomicU32();
        }
        // freeWordCount lives in the u16 of the topmost entry (PTE 511).
        Atomic<uint16_t>& freeWordCount() {
            return metaAt(kEntryCount - 1).atomicU16();
        }

        static size_t dirtyWordCount() {
            return divideAndRoundUp(arch::processorCount(), static_cast<size_t>(64));
        }
        static size_t dirtyCPUWord(size_t cpu) { return cpu / 64; }
        static uint64_t dirtyCPUBit(size_t cpu) { return uint64_t{1} << (cpu % 64); }

        void reserveEntry(size_t index) {
            const auto wordIndex = divideAndRoundDown(index, static_cast<size_t>(wordWidth));
            const uint32_t clearMask = ~(1u << (index % wordWidth));
            const uint32_t prevAlloc = allocWord(wordIndex);
            allocWord(wordIndex) = prevAlloc & clearMask;
            if (prevAlloc && !allocWord(wordIndex))
                freeWordCount().sub_fetch(1, RELAXED);
            const uint32_t prevFree = freeWord(wordIndex).load(RELAXED);
            freeWord(wordIndex).fetch_and(clearMask, RELAXED);
            if (prevFree && !freeWord(wordIndex).load(RELAXED))
                freeWordCount().sub_fetch(1, RELAXED);
        }

        // ── Lifetime ─────────────────────────────────────────────────────────

        // Must be called once before any claim/free.  Marks every usable slot free
        // and every reserved (bitmap) slot permanently unavailable.
        LeafPageTableWrapper() {
            memset(&table, 0, sizeof(table));
            freeWordCount().store(kBitmapWords, RELAXED);
            for (size_t i = 0; i < kBitmapWords; i++)
                allocWord(i) = UINT32_MAX;
            for (size_t i = kUsableEntries; i < kEntryCount; i++)
                reserveEntry(i);
            const size_t dirtyWords = dirtyWordCount();
            for (size_t i = 0; i < dirtyWords; i++)
                reserveEntry(i);
        }

        // ── Alloc / free ─────────────────────────────────────────────────────

        struct ClaimResult {
            LeafPTE* entry;    // nullptr when the table is full
            bool becameFull;   // true when this claim caused freeWordCount → 0
        };

        // Claim a free entry.  Allocator-CPU only (no concurrent callers).
        // Pass 0: scan allocBitmap with no atomics.
        // Pass 1: drain freeBitmap into allocBitmap, then scan again.
        ClaimResult claimFreeEntry() {
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1) {
                    bool anyDrained = false;
                    for (size_t w = 0; w < kBitmapWords; w++) {
                        const uint32_t freed = freeWord(w).exchange(0, ACQ_REL);
                        if (freed) { allocWord(w) |= freed; anyDrained = true; }
                    }
                    if (!anyDrained) return {nullptr, false};
                }
                for (size_t w = 0; w < kBitmapWords; w++) {
                    if (!allocWord(w)) continue;
                    const int bit = __builtin_ctz(allocWord(w));
                    allocWord(w) &= allocWord(w) - 1;            // clear lowest set bit
                    LeafPTE* entry = &table[w * wordWidth + static_cast<size_t>(bit)];
                    bool becameFull = false;
                    if (!allocWord(w)) {
                        const uint16_t prev = freeWordCount().fetch_sub(1, ACQ_REL);
                        becameFull = (prev == 1);
                    }
                    return {entry, becameFull};
                }
            }
            return {nullptr, false};
        }

        // Free a previously claimed entry.  May be called from any CPU.
        // Returns true if the table transitioned from full to available.
        bool freeEntry(LeafPTE* entry) {
            const size_t index = static_cast<size_t>(entry - table.data);
            const size_t w     = index / wordWidth;
            const uint32_t bit = uint32_t{1} << (index % wordWidth);
            const uint32_t prev = freeWord(w).fetch_or(bit, RELEASE);
            assert((prev & bit) == 0, "Double-free in page table");
            if (prev == 0) {
                const uint16_t prevCount = freeWordCount().fetch_add(1, ACQ_REL);
                return prevCount == 0;
            }
            return false;
        }
    };
    // ── Upper-level page table wrapper ──────────────────────────────────────
    //
    // Wraps any non-leaf page table level with a single 512-bit atomic occupancy
    // bitmap embedded in the topmost 16 reserved entries:
    //
    //   PTEs 496–511  bitmap words 0–15  (Atomic<uint32_t>, all mutators)
    //   PTE  511      also holds freeWordCount in its uint16_t field
    //
    // 496 usable entries (0–495).  Word 15 is partial: bits 0–15 cover usable
    // entries 480–495; bits 16–31 are permanently 0 (reserved entries 496–511).
    //
    // freeWordCount ∈ [0, 16]: counts words with ≥1 free bit.
    //
    // Three-operation API (intentionally separated so the caller can write a
    // subtable pointer into the entry between find and mark):
    //   findFreeEntry   — read-only scan, no bitmap mutation
    //   markEntryUsed   — XOR bit 1→0, decrement freeWordCount if word emptied
    //   markEntryFree   — XOR bit 0→1, increment freeWordCount if word was empty

    constexpr size_t kUpperUsable      = kEntryCount - kBitmapWords; // 496
    constexpr size_t kUpperBitmapStart = kUpperUsable;               // 496

    template <size_t level>
    struct UpperPageTableWrapper {
        static_assert(level < leafLevel, "UpperPageTableWrapper requires a non-leaf level");

        using UpperPTE  = arch::PTE<level>;
        using UpperMeta = arch::PTEMetadataEntry<arch::pageTableDescriptor.levels[level]>;

        arch::PageTable<level> table;

        // ── Internal accessors ───────────────────────────────────────────────

        UpperMeta& metaAt(size_t index) {
            return *reinterpret_cast<UpperMeta*>(&table[index]);
        }
        Atomic<uint32_t>& bitmapWord(size_t i) {
            return metaAt(kUpperBitmapStart + i).atomicU32();
        }
        Atomic<uint16_t>& freeWordCount() {
            return metaAt(kEntryCount - 1).atomicU16();
        }

        void reserveEntry(size_t index) {
            const size_t w = index / wordWidth;
            const size_t b = index % wordWidth;
            bitmapWord(w).fetch_and(~(uint32_t{1} << b), RELAXED);
        }

        // ── Lifetime ─────────────────────────────────────────────────────────

        UpperPageTableWrapper() {
            memset(&table, 0, sizeof(table));
            for (size_t i = 0; i < kBitmapWords; i++)
                bitmapWord(i).store(UINT32_MAX, RELAXED);
            for (size_t i = kUpperUsable; i < kEntryCount; i++)
                reserveEntry(i);
            uint16_t freeWords = 0;
            for (size_t i = 0; i < kBitmapWords; i++)
                if (bitmapWord(i).load(RELAXED)) freeWords++;
            freeWordCount().store(freeWords, RELAXED);
        }

        // ── Find / mark used / mark free ─────────────────────────────────────

        // Scan for any free entry.  Allocator-CPU only; does not mutate the bitmap.
        // Returns nullptr when the table is full.
        UpperPTE* findFreeEntry() {
            for (size_t w = 0; w < kBitmapWords; w++) {
                const uint32_t val = bitmapWord(w).load(ACQUIRE);
                if (!val) continue;
                return &table[w * wordWidth + static_cast<size_t>(__builtin_ctz(val))];
            }
            return nullptr;
        }

        // Mark a free entry as used (XOR 1→0).  Allocator-CPU only.
        // Returns true if the table transitioned to full.
        bool markEntryUsed(UpperPTE* entry) {
            const size_t index = static_cast<size_t>(entry - table.data);
            const size_t w     = index / wordWidth;
            const uint32_t bit = uint32_t{1} << (index % wordWidth);
            const uint32_t old = bitmapWord(w).fetch_xor(bit, ACQ_REL);
            //assert(old & bit, "markEntryUsed: entry was not free");
            if ((old & ~bit) == 0) {
                const uint16_t prev = freeWordCount().fetch_sub(1, ACQ_REL);
                return prev == 1;
            }
            return false;
        }

        // Mark a used entry as free (XOR 0→1).  May be called from any CPU.
        // Returns true if the table transitioned from full to available.
        bool markEntryFree(UpperPTE* entry) {
            const size_t index = static_cast<size_t>(entry - table.data);
            const size_t w     = index / wordWidth;
            const uint32_t bit = uint32_t{1} << (index % wordWidth);
            const uint32_t old = bitmapWord(w).fetch_xor(bit, RELEASE);
            //assert(!(old & bit), "markEntryFree: entry was already free");
            if (old == 0) {
                const uint16_t prevCount = freeWordCount().fetch_add(1, ACQ_REL);
                return prevCount == 0;
            }
            return false;
        }
    };

    template <size_t level>
    struct PageTableWrapper : UpperPageTableWrapper<level> {};

    template <>
    struct PageTableWrapper<leafLevel> : LeafPageTableWrapper {};

    static_assert([]() {
        for (size_t i = 1; i < arch::pageTableDescriptor.LEVEL_COUNT; i++)
            if (arch::pageTableDescriptor.entryCount[i] != arch::pageTableDescriptor.entryCount[0])
                return false;
        return true;
    }(), "VMSubstrateArena requires uniform entry count across all page table levels");

} // VMSubstrateHelper

namespace kernel::mm::VMSubstrate {

    arch::PageTable<pageTableLevelForKMemRegion() - 1> vmmArenaTable;

    Atomic<size_t> freeArenaIndex = 0;
    WITH_GLOBAL_CONSTRUCTOR(Spinlock, arenaCreationLock);

    template <size_t level>
    phys_addr initializePageTable(arch::ProcessorID cpu, phys_addr subtable = phys_addr(nullptr)) requires (level >= pageTableLevelForKMemRegion()) && (level < arch::pageTableDescriptor.LEVEL_COUNT){
        const auto ptaddr = PageAllocator::allocateSmallPage(cpu);
        TempWindow<VMSubstrateHelper::PageTableWrapper<level>> window(ptaddr);
        auto* pageTablePtr = new (&*window)VMSubstrateHelper::PageTableWrapper<level>();
        using Flag = arch::PageEntryFlag;
        constexpr auto kSubtableFlags = Flag::Write | Flag::Global | Flag::NoExecute;
        if constexpr (level == pageTableLevelForKMemRegion()) {
            auto& selfRef = pageTablePtr->table[0];
            selfRef = arch::PTE<level>::subtableEntry(ptaddr, kSubtableFlags);
            pageTablePtr->reserveEntry(0);  // self-ref: permanently off-limits for allocation
            auto& first = pageTablePtr->table[1];
            first = arch::PTE<level>::subtableEntry(subtable, kSubtableFlags);

            assert(selfRef.isPresent(), "AAAA");
            assert(first.isPresent(), "AAAA");
            // PD[1] keeps its bitmap bit = 1: the newly installed PT has free entries
        } else if constexpr (level == arch::pageTableDescriptor.LEVEL_COUNT - 1) {
            // Install per-CPU dirty-bitmap pages at entries 0..dirtyWordCount()-1.
            const size_t dirtyWords = VMSubstrateHelper::LeafPageTableWrapper::dirtyWordCount();
            for (size_t dw = 0; dw < dirtyWords; dw++) {
                const phys_addr dirtyPhys = PageAllocator::allocateSmallPage(cpu);
                pageTablePtr->table[dw] = arch::PTE<level>::leafEntry(dirtyPhys, kSubtableFlags);
            }
        } else {
            auto& entry = pageTablePtr->table[0];
            entry = arch::PTE<level>::subtableEntry(subtable, kSubtableFlags);
        }
        return ptaddr;
    }

    // Recursively initializes the full page table chain from the leaf up to topLevel,
    // feeding each level's physical address as the subtable of the level above.
    template <size_t level>
    phys_addr initializeArenaChain(arch::ProcessorID cpu) requires (level >= pageTableLevelForKMemRegion()) && (level < arch::pageTableDescriptor.LEVEL_COUNT) {
        if constexpr (level == arch::pageTableDescriptor.LEVEL_COUNT - 1) {
            return initializePageTable<level>(cpu);
        } else {
            return initializePageTable<level>(cpu, initializeArenaChain<level + 1>(cpu));
        }
    }

    virt_addr arenaVirtualBase(size_t index) {
        constexpr virt_addr substrateBase = arch::pageTableDescriptor.canonicalizeVirtualAddress(
            virt_addr{static_cast<uint64_t>(VMM_SUBSTRATE_ROOT_INDEX)
                      << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion() - 1)});
        return substrateBase + index * getKernelMemRegionSize();
    }

    struct VMSubstrateArena {
        template <size_t n>
        using PT = VMSubstrateHelper::PageTableWrapper<n>;
        template <size_t n>
        using PTE = arch::PTE<n>;
        using RootTable = PT<pageTableLevelForKMemRegion()>;

        static VMSubstrateArena forCurrentCPU() {
            return VMSubstrateArena{arenaVirtualBase(static_cast<size_t>(arch::getCurrentProcessorID()))};
        }

        static VMSubstrateArena forPointer(void* ptr) {
            const auto ptrAddr = reinterpret_cast<uint64_t>(ptr);
            return VMSubstrateArena{virt_addr{roundDownToNearestMultiple(ptrAddr, getKernelMemRegionSize())}};
        }

    private:
        uint64_t baseAddr_;

        explicit VMSubstrateArena(virt_addr base) : baseAddr_(base.value) {}

        [[nodiscard]] virt_addr base() const { return virt_addr{baseAddr_}; }
        [[nodiscard]] RootTable& root() const {
            return *reinterpret_cast<RootTable*>(base().value);
        }

        [[nodiscard]] void* getChildAddr(void* entryPtr) const {
            const auto addr = reinterpret_cast<uint64_t>(entryPtr);
            const auto offset = addr - base().value;
            const auto outAddr = offset * arch::pageTableDescriptor.entryCount[0] + base().value;
            return reinterpret_cast<void*>(outAddr);
        }

        // Recursive allocator: walks the arena page table tree from level n down to the leaf,
        // installing new sub-tables as needed.  Returns the mapped virtual address on success
        // or nullptr if the arena is full.  outBecameFull is set to true if this call caused
        // the table at level n to transition from available → full.
        // mmioAddr: non-zero → map that physical address with CacheDisable (for MMIO);
        //           zero    → allocate a new physical page normally.
        template <size_t n>
        void* allocFromLevel(PT<n>& table, bool& outBecameFull, phys_addr mmioAddr = phys_addr(UINT64_MAX)) {
            using Flag = arch::PageEntryFlag;
            constexpr auto kFlags = Flag::Write | Flag::Global | Flag::NoExecute;

            if constexpr (n == VMSubstrateHelper::leafLevel) {
                auto [entry, becameFull] = table.claimFreeEntry();
                if (!entry) { outBecameFull = false; return nullptr; }
                const bool isMMIO = mmioAddr.value != UINT64_MAX;
                const phys_addr paddr = isMMIO ? mmioAddr
                    : PageAllocator::allocateSmallPage(arch::getCurrentProcessorID());
                *entry = PTE<n>::leafEntry(paddr, isMMIO ? kFlags | Flag::CacheDisable : kFlags);
                // Notify all other CPUs that this mapping is new and their TLBs are stale.
                using LPT = VMSubstrateHelper::LeafPageTableWrapper;
                void* result = getChildAddr(entry);
                const auto resultAddr = reinterpret_cast<uint64_t>(result);
                const auto tableBase = roundDownToNearestMultiple(resultAddr, arch::bigPageSize);
                const size_t k_abs = (resultAddr - tableBase) / arch::smallPageSize;
                const size_t myCPU = arch::getCurrentProcessorID();
                for (size_t dw = 0; dw < LPT::dirtyWordCount(); dw++) {
                    const uint64_t mask = (LPT::dirtyCPUWord(myCPU) == dw)
                        ? UINT64_MAX & ~LPT::dirtyCPUBit(myCPU)
                        : UINT64_MAX;
                    if (mask) {
                        auto& dirtyEntry = *reinterpret_cast<Atomic<uint64_t>*>(
                            tableBase + dw * arch::smallPageSize + k_abs * sizeof(uint64_t));
                        dirtyEntry.fetch_or(mask, RELEASE);
                    }
                }
                outBecameFull = becameFull;
                return result;
            } else {
                PTE<n>* entry = table.findFreeEntry();
                if (!entry) { outBecameFull = false; return nullptr; }

                auto* child = reinterpret_cast<PT<n + 1>*>(getChildAddr(entry));
                if (!entry->isPresent()) {
                    const phys_addr physAddr = PageAllocator::allocateSmallPage(arch::getCurrentProcessorID());
                    *entry = PTE<n>::subtableEntry(physAddr, kFlags);
                    arch::invlpg(virt_addr{reinterpret_cast<uint64_t>(child)});
                    new (child) PT<n + 1>();
                    if constexpr (n + 1 == VMSubstrateHelper::leafLevel) {
                        using LPT = VMSubstrateHelper::LeafPageTableWrapper;
                        const auto tableBase = reinterpret_cast<uint64_t>(getChildAddr(child));
                        for (size_t dw = 0; dw < LPT::dirtyWordCount(); dw++) {
                            const phys_addr dirtyPhys = PageAllocator::allocateSmallPage(
                                arch::getCurrentProcessorID());
                            child->table[dw] = PTE<n + 1>::leafEntry(dirtyPhys, kFlags);
                            arch::invlpg(virt_addr{tableBase + dw * arch::smallPageSize});
                        }
                    }
                }

                bool childBecameFull = false;
                void* result = allocFromLevel<n + 1>(*child, childBecameFull, mmioAddr);
                if (childBecameFull)
                    outBecameFull = table.markEntryUsed(entry);
                else
                    outBecameFull = false;
                return result;
            }
        }

        // Propagates a "became available" signal up the tree after a child table transitions
        // from full to having free entries.  Intermediate tables are never freed.
        template <size_t n>
        void propagateAvailability(PT<n + 1>& childTable) {
            constexpr size_t entryCount = arch::pageTableDescriptor.entryCount[0];
            const auto childAddr = reinterpret_cast<uint64_t>(&childTable);
            const auto parentEntryAddr = (childAddr - base().value) / entryCount + base().value;
            auto& parentEntry = *reinterpret_cast<PTE<n>*>(parentEntryAddr);
            const auto parentTableAddr = roundDownToNearestMultiple(parentEntryAddr, sizeof(PT<n>));
            PT<n>& parentTable = *reinterpret_cast<PT<n>*>(parentTableAddr);
            const bool becameAvailable = parentTable.markEntryFree(&parentEntry);
            if constexpr (n > pageTableLevelForKMemRegion()) {
                if (becameAvailable)
                    propagateAvailability<n - 1>(parentTable);
            }
        }

    public:
        void* allocPage() {
            bool ignored = false;
            const auto out = allocFromLevel<pageTableLevelForKMemRegion()>(root(), ignored);
            arch::invlpg(virt_addr(out));
            return out;
        }

        void* mapMMIOPage(phys_addr paddr) {
            assert(paddr.value % arch::smallPageSize == 0, "Misaligned MMIO physical address");
            bool ignored = false;
            const auto out = allocFromLevel<pageTableLevelForKMemRegion()>(root(), ignored, paddr);
            arch::invlpg(virt_addr(out));
            return out;
        }

        void freePage(void* ptr) {
            constexpr size_t entryCount = arch::pageTableDescriptor.entryCount[0];
            const auto ptrAddr = reinterpret_cast<uint64_t>(ptr);
            const auto leafPTEAddr = (ptrAddr - base().value) / entryCount + base().value;
            auto& leafEntry = *reinterpret_cast<PTE<VMSubstrateHelper::leafLevel>*>(leafPTEAddr);

            const phys_addr physAddr = leafEntry.getPhysicalAddress();
            leafEntry = PTE<VMSubstrateHelper::leafLevel>{};
            arch::invlpg(virt_addr{ptrAddr});
            PageAllocator::freeSmallPage(physAddr);

            const auto leafTableAddr = roundDownToNearestMultiple(leafPTEAddr, sizeof(PT<VMSubstrateHelper::leafLevel>));
            auto& leafTable = *reinterpret_cast<PT<VMSubstrateHelper::leafLevel>*>(leafTableAddr);
            if (leafTable.freeEntry(&leafEntry))
                propagateAvailability<VMSubstrateHelper::leafLevel - 1>(leafTable);
        }
    };

    void* allocPage() {
        return VMSubstrateArena::forCurrentCPU().allocPage();
    }

    void freePage(void* ptr) {
        VMSubstrateArena::forPointer(ptr).freePage(ptr);
    }

    void* mapMMIOPage(phys_addr paddr) {
        return VMSubstrateArena::forCurrentCPU().mapMMIOPage(paddr);
    }

    void* createArena(arch::ProcessorID cpu) {
        LockGuard arenaGuard(arenaCreationLock);
        const phys_addr topAddr = initializeArenaChain<pageTableLevelForKMemRegion()>(cpu);
        using Flag = arch::PageEntryFlag;
        constexpr auto kSubtableFlags = Flag::Write | Flag::Global | Flag::NoExecute;
        const size_t index = freeArenaIndex.fetch_add(1, RELAXED);
        vmmArenaTable[index] = arch::PTE<pageTableLevelForKMemRegion() - 1>::subtableEntry(topAddr, kSubtableFlags);
        return reinterpret_cast<void*>(arenaVirtualBase(index).value);
    }

    void ensureTLBEntryFresh(void* ptr) {
        const auto ptrAddr = reinterpret_cast<uint64_t>(ptr);
        const auto tableBase = roundDownToNearestMultiple(ptrAddr, arch::bigPageSize);
        const size_t k_abs = (ptrAddr - tableBase) / arch::smallPageSize;
        const size_t myCPU = arch::getCurrentProcessorID();
        using LPT = VMSubstrateHelper::LeafPageTableWrapper;
        const size_t dw = LPT::dirtyCPUWord(myCPU);
        const uint64_t bit = LPT::dirtyCPUBit(myCPU);
        auto& dirtyEntry = *reinterpret_cast<Atomic<uint64_t>*>(
            tableBase + dw * arch::smallPageSize + k_abs * sizeof(uint64_t));
        if (dirtyEntry.load(ACQUIRE) & bit) {
            arch::invlpg(virt_addr{ptrAddr});
            dirtyEntry.fetch_and(~bit, RELAXED);
        }
    }

    bool init() {
        using Flag = arch::PageEntryFlag;
        constexpr auto kSubtableFlags = Flag::Write | Flag::Global | Flag::NoExecute;
        bootPageTable[VMM_SUBSTRATE_ROOT_INDEX] = arch::PTE<0>::subtableEntry(
            early_boot_virt_to_phys(virt_addr(&vmmArenaTable)),
            kSubtableFlags);
        assert(bootPageTable[VMM_SUBSTRATE_ROOT_INDEX].isPresent(), "AAAA");
        for (size_t i = 0; i < arch::processorCount(); i++) {
            createArena(static_cast<arch::ProcessorID>(i));
        }
        arch::flushTLB();
        return true;
    }
}
