//
// Created by Spencer Martin on 1/14/26.
//

#ifndef CROCOS_PAGETABLESPECIFICATION_H
#define CROCOS_PAGETABLESPECIFICATION_H

#include <core/utility.h>
#include <assert.h>
#include <kernel.h>
#include <core/math.h>

#define PARANOID_PAGING_ASSERTIONS

namespace arch{
    using PageTableEntryBit = size_t;

    static constexpr size_t BIT_NOT_PRESENT = ~0ULL;

    struct PageTableLevelDescriptor {
        struct PropertyBits {
            PageTableEntryBit userAccessible;
            PageTableEntryBit writable;
            PageTableEntryBit executable;
            PageTableEntryBit global;
            PageTableEntryBit accessed;
            PageTableEntryBit dirty;

            bool writeableOnOne;
            bool executeOnOne;
            bool globalOnOne;
        };

        struct EntryEncoding {
            PropertyBits properties;
            size_t physAddrLowestBit;
            size_t physAddrTotalBits;
            PageTableEntryBit addrStartInEntry;

            [[nodiscard]] constexpr size_t physAddrMask() const {
                const size_t out = (1ULL << physAddrTotalBits) - 1;
                return out << physAddrLowestBit;
            }

            [[nodiscard]] constexpr size_t entryAddrMask() const {
                const size_t out = (1ULL << physAddrTotalBits) - 1;
                return out << addrStartInEntry;
            }
        };

        constexpr static EntryEncoding EMPTY_ENTRY = {};

        bool canBeLeaf;
        bool canBeSubtable;
        PageTableEntryBit leafIndexBit;
        bool isLeafOnOne;
        size_t entryWidth;
        PageTableEntryBit present;
        EntryEncoding subtableEncoding;
        EntryEncoding leafEncoding;

        [[nodiscard]] constexpr EntryEncoding getEncoding(bool isLeaf) const {
            if (isLeaf) {
                return leafEncoding;
            }
            return subtableEncoding;
        }
    };

    template <size_t levelCount>
    struct PageTableDescriptor {
        static constexpr auto LEVEL_COUNT = levelCount;
        //level[0] should be the top level page table, so on amd64 this would be the pml4 (or pml5 if you're using 5 level paging)
        PageTableLevelDescriptor levels[levelCount];
        size_t entryCount[levelCount]; //These should be powers of 2

        [[nodiscard]] constexpr size_t getTableSize(size_t level) const {
            return entryCount[level] * levels[level].entryWidth / 8;
        }

        [[nodiscard]] constexpr size_t getVirtualAddressBitCount(const size_t level = 0) const {
            size_t bitCount = 0;
            for (size_t i = level; i < LEVEL_COUNT; i++) {
                bitCount += log2floor(entryCount[i]);
            }
            bitCount += levels[LEVEL_COUNT - 1].leafEncoding.physAddrLowestBit;
            return bitCount;
        }

        [[nodiscard]] constexpr size_t getVirtualAddressMask() const {
            return (1 << getVirtualAddressBitCount()) - 1;
        }

        [[nodiscard]] constexpr kernel::mm::virt_addr canonicalizeVirtualAddress(const kernel::mm::virt_addr addr) const {
            const auto shift = sizeof(addr.value) * 8 - getVirtualAddressBitCount();
            using UnsignedType = SmallestUInt_t<sizeof(addr.value) * 8>;
            using SignedType = SmallestInt_t<sizeof(addr.value) * 8>;
            const UnsignedType shifted = (addr.value << shift);
            const auto sgn = static_cast<SignedType>(shifted) >> shift;
            return kernel::mm::virt_addr(static_cast<UnsignedType>(sgn));
        }

        [[nodiscard]] constexpr size_t getTableSize(size_t level) {
            return entryCount[level] * levels[level].entryWidth / 8;
        }
    };

    constexpr bool isPTESizeValid(size_t size){
        switch (size) {
            case 8:
            case 16:
            case 32:
            case 64: return true;
            default: return false;
        }
    }

    template <PageTableLevelDescriptor encoding>
    struct PageTableEntry {
        static_assert(isPTESizeValid(encoding.entryWidth));
        using DataType = SmallestUInt_t<encoding.entryWidth>;
        DataType data;

        [[nodiscard]] static constexpr bool hasUserAccessibleBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.userAccessible != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasUserAccessibleBit() const {
            return hasUserAccessibleBit(isLeafEntry());
        }

        [[nodiscard]] static constexpr bool hasGlobalBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.global != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasGlobalBit() const {
            return hasGlobalBit(isLeafEntry());
        }

        [[nodiscard]] static constexpr bool hasAccessedBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.accessed != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasAccessedBit() const {
            return hasAccessedBit(isLeafEntry());
        }

        [[nodiscard]] static constexpr bool hasWriteBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.writable != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasWriteBit() const {
            return hasWriteBit(isLeafEntry());
        }

        [[nodiscard]] static constexpr bool hasExecuteBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.executable != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasExecuteBit() const {
            return hasExecuteBit(isLeafEntry());
        }

        [[nodiscard]] static constexpr bool hasDirtyBit(const bool leaf) {
            return encoding.getEncoding(leaf).properties.dirty != BIT_NOT_PRESENT;
        }

        [[nodiscard]] constexpr bool hasDirtyBit() const {
            return hasDirtyBit(isLeafEntry());
        }

        [[nodiscard]] constexpr static PageTableEntry subtableEntry(kernel::mm::phys_addr addr) {
            //Ensure the address is properly aligned
#ifdef PARANOID_PAGING_ASSERTIONS
            assert((addr.value & (~encoding.subtableEncoding.physAddrMask())) == 0, "");
            assert(encoding.canBeSubtable, "There is no sensible subtable entry at this level");
#endif
            DataType out = (addr.value >> encoding.subtableEncoding.physAddrLowestBit) << encoding.subtableEncoding.addrStartInEntry;
            DataType subtableBit = 1ULL << encoding.leafIndexBit;
            if (!encoding.isLeafOnOne) {
                out |= subtableBit;
            }
            out |= 1ULL << encoding.present;
            return {out};
        }

        [[nodiscard]] constexpr static PageTableEntry leafEntry(kernel::mm::phys_addr addr) {
            //Ensure the address is properly aligned
#ifdef PARANOID_PAGING_ASSERTIONS
            assert((addr.value & (~encoding.leafEncoding.physAddrMask())) == 0, "");
            assert(encoding.canBeLeaf, "There is no sensible leaf entry at this level");
#endif
            DataType out = (addr.value >> encoding.leafEncoding.physAddrLowestBit) << encoding.leafEncoding.addrStartInEntry;
            DataType subtableBit = 1ULL << encoding.leafIndexBit;
            if (encoding.isLeafOnOne) {
                out |= subtableBit;
            }
            out |= 1ULL << encoding.present;
            return {out};
        }

        [[nodiscard]] constexpr bool isLeafEntry() const {
            if (!encoding.canBeSubtable) {
                return true;
            }
            if (!encoding.canBeLeaf) {
                return false;
            }
            return ((data >> encoding.leafIndexBit) & 1) == encoding.isLeafOnOne;
        }

        [[nodiscard]] constexpr bool isSubtableEntry() const {
            return !isLeafEntry();
        }

        void enableWrite(const bool enabled = true) {
            const PageTableEntryBit writeBit = encoding.getEncoding(isLeafEntry()).properties.writable;
#ifdef PARANOID_PAGING_ASSERTIONS
            assert(hasWriteBit(), "Cannot enable write on this entry");
#endif
            DataType bit = (1ULL << writeBit);
            DataType newData = data & (~bit);
            const bool writeOnOne = encoding.getEncoding(isLeafEntry()).properties.writeableOnOne;
            if (writeOnOne == enabled) {
                newData |= bit;
            }
            data = newData;
        }

        void enableExecute(const bool enabled = true) {
            const PageTableEntryBit executeBit = encoding.getEncoding(isLeafEntry()).properties.executable;
#ifdef PARANOID_PAGING_ASSERTIONS
            assert(hasExecuteBit(), "Cannot enable execute on this entry");
#endif
            DataType bit = (1ULL << executeBit);
            DataType newData = data & (~bit);
            const bool executeOnOne = encoding.getEncoding(isLeafEntry()).properties.executeOnOne;
            if (executeOnOne == enabled) {
                newData |= bit;
            }
            data = newData;
        }

        void markPresent(const bool enabled = true) {
            DataType bit = 1ULL << encoding.present;
            DataType newData = data & (~bit);
            if (enabled) {
                newData |= bit;
            }
            data = newData;
        }

        void markGlobal(const bool global = true) {
            const PageTableEntryBit globalBit = encoding.getEncoding(isLeafEntry()).properties.global;
#ifdef PARANOID_PAGING_ASSERTIONS
            assert(hasGlobalBit(), "Cannot mark global on this entry");
#endif
            DataType bit = (1ULL << globalBit);
            DataType newData = data & (~bit);
            const bool globalOnOne = encoding.getEncoding(isLeafEntry()).properties.globalOnOne;
            if (globalOnOne == global) {
                newData |= bit;
            }
            data = newData;
        }

        void setDirty(const bool dirty = true) {
            const PageTableEntryBit dirtyBit = encoding.getEncoding(isLeafEntry()).properties.dirty;
#ifdef PARANOID_PAGING_ASSERTIONS
            assert(hasDirtyBit(), "Cannot mark global on this entry");
#endif
            DataType bit = (1ULL << dirtyBit);
            DataType newData = data & (~bit);
            if (dirty) {
                newData |= bit;
            }
            data = newData;
        }

        void markClean() {
            setDirty(false);
        }

        void setAccessedFlag(const bool accessed = true) {
            const PageTableEntryBit accessedBit = encoding.getEncoding(isLeafEntry()).properties.accessed;
#ifdef PARANOID_PAGING_ASSERTIONS
            assert(hasDirtyBit(), "Cannot mark global on this entry");
#endif
            DataType bit = (1ULL << accessedBit);
            DataType newData = data & (~bit);
            if (accessed) {
                newData |= bit;
            }
            data = newData;
        }

        void clearAccessedFlag() {
            setAccessedFlag(false);
        }

        [[nodiscard]] constexpr bool isPresent() const {
            return (data & (1ULL << encoding.present)) != 0;
        }

        [[nodiscard]] constexpr bool isGlobal() const {
            return !!(data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.global)) ==
                encoding.getEncoding(isLeafEntry()).properties.globalOnOne;
        }

        [[nodiscard]] constexpr bool isUserAccessible() const {
            return (data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.userAccessible)) != 0;
        }

        [[nodiscard]] constexpr bool canWrite() const {
            return !!(data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.writable)) ==
                encoding.getEncoding(isLeafEntry()).properties.writeableOnOne;
        }

        [[nodiscard]] constexpr bool canExecute() const {
            return !!(data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.executable)) ==
                encoding.getEncoding(isLeafEntry()).properties.executeOnOne;
        }

        [[nodiscard]] constexpr bool isDirty() const {
            return (data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.dirty)) != 0;
        }

        [[nodiscard]] constexpr bool wasAccessed() const {
            return (data & (1ULL << encoding.getEncoding(isLeafEntry()).properties.accessed)) != 0;
        }

        [[nodiscard]] constexpr kernel::mm::phys_addr getPhysicalAddress() const {
            auto addrBits = (data & encoding.getEncoding(isLeafEntry()).entryAddrMask()) >> encoding.getEncoding(isLeafEntry()).addrStartInEntry;
            return kernel::mm::phys_addr{addrBits << encoding.getEncoding(isLeafEntry()).physAddrLowestBit};
        }

        void setPhysicalAddress(kernel::mm::phys_addr addr) {
#ifdef PARANOID_PAGING_ASSERTIONS
            assert((addr.value & ~encoding.getEncoding(isLeafEntry()).physAddrMask()) == 0, "Physical address is not properly aligned");
#endif
            auto addrBits = addr.value >> encoding.getEncoding(isLeafEntry()).physAddrLowestBit;
            data = (data & ~(encoding.getEncoding(isLeafEntry()).entryAddrMask())) | (addrBits << encoding.getEncoding(isLeafEntry()).addrStartInEntry);
        }
    } __attribute__ ((packed));
}

#endif //CROCOS_PAGETABLESPECIFICATION_H