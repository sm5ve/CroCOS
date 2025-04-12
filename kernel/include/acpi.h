//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_ACPI_H
#define CROCOS_ACPI_H

#include "stdint.h"
#include <lib/ds/Vector.h>
#include <mm.h>
#include <lib/str.h>

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
namespace kernel::acpi{
    extern struct SDTHeader* rsdt;
    //Basic ACPI table structures from https://wiki.osdev.org/RSDP and related pages
    struct RSDP {
        char signature[8]; //"RSD PTR "
        uint8_t checksum;
        char oemid[6];
        uint8_t revision;
        uint32_t rsdtAddress; //deprecated
        //Only exists if revision == 2
        uint32_t length;
        uint64_t xsdtAddress;
        uint8_t extendedChecksum;
        uint8_t reserved[3];
    } __attribute__ ((packed));

    struct SDTHeader {
        char signature[4];
        uint32_t length;
        uint8_t revision;
        uint8_t checksum;
        char oemid[6];
        char oemTableID[8];
        uint32_t oemRevision;
        uint32_t creatorID;
        uint32_t creatorRevision;
    } __attribute__ ((packed));

    struct RSDT {
        struct SDTHeader h;
        uint32_t tablePointer;
    } __attribute__ ((packed));

    struct XSDT {
        struct SDTHeader h;
        uint64_t tablePointer;
    } __attribute__ ((packed));

    struct MADTEntryHeader{
        uint8_t type;
        uint8_t length;
    } __attribute__ ((packed));

    struct MADT_LAPIC_Entry{
        struct MADTEntryHeader h;
        uint8_t acpiProcessorID;
        uint8_t apicID;
        uint32_t flags;
    } __attribute__ ((packed));

    struct MADT_IOAPIC_Entry{
        struct MADTEntryHeader h;
        uint8_t ioapicID;
        uint8_t reserved;
        uint32_t ioapicAddress;
        uint32_t gsiBase;
    } __attribute__ ((packed));

    struct MADT_IOAPIC_Source_Override_Entry{
        struct MADTEntryHeader h;
        uint8_t busSource;
        uint8_t irqSource;
        uint32_t gsi;
        uint16_t flags;
    } __attribute__ ((packed));

    struct MADT_IOAPIC_NMI_Source_Entry{
        struct MADTEntryHeader h;
        uint8_t nmiSource;
        uint8_t reserved;
        uint16_t flags;
        uint32_t gsi;
    } __attribute__ ((packed));

    struct MADT_LAPIC_NMI_Entry{
        struct MADTEntryHeader h;
        uint8_t acpiProcessorID;
        uint16_t flags;
        uint8_t lintNo;
    } __attribute__ ((packed));

    template <typename TableType>
    struct MADTEntryType {
        static_assert(false, "Unsupported ACPI table type");
    };

    template <>
    struct MADTEntryType<MADT_LAPIC_Entry> {
        static constexpr uint8_t type = 0;
    };

    template <>
    struct MADTEntryType<MADT_IOAPIC_Entry> {
        static constexpr uint8_t type = 1;
    };

    template <>
    struct MADTEntryType<MADT_IOAPIC_Source_Override_Entry> {
        static constexpr uint8_t type = 2;
    };

    template <>
    struct MADTEntryType<MADT_IOAPIC_NMI_Source_Entry> {
        static constexpr uint8_t type = 3;
    };

    template <>
    struct MADTEntryType<MADT_LAPIC_NMI_Entry> {
        static constexpr uint8_t type = 4;
    };

    template <typename T>
    struct MADTIterator{
    public:
        MADTIterator(MADTEntryHeader& beginning, MADTEntryHeader& end)
        : s(&beginning), e(&end) {
            advanceToNextEntryForType();
        }

        T& operator*() const { return *(T*)s; }

        MADTIterator& operator++() {
            s = (MADTEntryHeader*)((uint64_t)s + (uint8_t)s -> length);
            advanceToNextEntryForType();
            return *this;
        }

        bool operator!=(const MADTIterator& other) const {
            return s != other.s;
        }

    private:
        void advanceToNextEntryForType() {
            while (s < e) {
                if(s -> type == MADTEntryType<T>::type){
                    break;
                }
                assert(s -> length > 0, "Malformed MADT!!!!");
                s = (MADTEntryHeader*)((uint64_t)s + s -> length);
            }
            assert(s <= e, "Malformed MADT!!!!");
        }

        MADTEntryHeader *s, *e;
    };

    template <typename T>
    struct MADTRange{
    public:
        MADTRange(MADTEntryHeader& beginning, MADTEntryHeader& end)
                : s(MADTIterator<T>(beginning, end)), e(MADTIterator<T>(end, end)) {
        }

        MADTIterator<T> begin() const { return s; }
        MADTIterator<T> end()   const { return e; }
    private:
        MADTIterator<T> s, e;
    };

    struct MADT {
        struct SDTHeader h;
        uint32_t lapicAddr;
        uint32_t flags;
        struct MADTEntryHeader tableEntries;
        size_t getEnabledProcessorCount();

        template <typename T>
        MADTRange<T> entries(){
            return MADTRange<T>(tableEntries, *(MADTEntryHeader*)((uint64_t)&h + h.length));
        }
    } __attribute__ ((packed));

    enum ACPIDiscoveryResult{
        NOT_FOUND,
        CHECKSUM_FAIL,
        MISMATCHED_SIGNATURE,
        SUCCESS
    };

    enum ACPIChecksumResult{
        FAIL = 0,
        PASS = 1
    };

    template <typename TableType>
    struct ACPISignature {
        static_assert(false, "Unsupported ACPI table type");
    };

    template <>
    struct ACPISignature<RSDT> {
        static constexpr const char* value = "RSDT";
    };

    template <>
    struct ACPISignature<XSDT> {
        static constexpr const char* value = "XSDT";
    };

    template <>
    struct ACPISignature<MADT> {
        static constexpr const char* value = "APIC";
    };

    ACPIDiscoveryResult tryFindACPI();
    ACPIChecksumResult verifyTableChecksum(SDTHeader* header);

    template<typename T>
    Vector<T*> getTables(){
        if(rsdt == nullptr){
            return Vector<T*>();
        }
        size_t tableCount = 0;
        if(startsWith(rsdt -> signature, "RSDT")){
            RSDT* root = (RSDT*)rsdt;
            //Our early allocator is a bump allocator, and in particular can't free. To avoid wasting memory, let's
            //count how big of a buffer we should preallocate. Probably overkill, but seems most correct.
            for(uint32_t* tablePhysAddress = &(root -> tablePointer); (uint64_t)tablePhysAddress <
            (uint64_t)root + (root -> h.length); tablePhysAddress++){
#ifdef __x86_64__
                SDTHeader* table = amd64::early_boot_phys_to_virt(mm::phys_addr((uint64_t)*tablePhysAddress)).as_ptr<SDTHeader>();
#endif
                if(startsWith(table -> signature, ACPISignature<T>::value) && verifyTableChecksum(table) == ACPIChecksumResult::PASS){
                    tableCount++;
                }
            }
            Vector<T*> out(tableCount);
            for(uint32_t* tablePhysAddress = &(root -> tablePointer); (uint64_t)tablePhysAddress <
            (uint64_t)root + (root -> h.length); tablePhysAddress++){
#ifdef __x86_64__
                SDTHeader* table = amd64::early_boot_phys_to_virt(mm::phys_addr(*tablePhysAddress)).as_ptr<SDTHeader>();
#endif
                if(startsWith(table -> signature, ACPISignature<T>::value) && verifyTableChecksum(table) == ACPIChecksumResult::PASS){
                    out.push((T*)table);
                }
            }
            return out;
        }
        else if(startsWith(rsdt -> signature, "XSDT")){
            XSDT* root = (XSDT*)rsdt;
            //Our early allocator is a bump allocator, and in particular can't free. To avoid wasting memory, let's
            //count how big of a buffer we should preallocate. Probably overkill, but seems most correct.
            for(uint64_t* tablePhysAddress = &(root -> tablePointer); (uint64_t)tablePhysAddress <
            (uint64_t)root + (root -> h.length); tablePhysAddress++){
#ifdef __x86_64__
                SDTHeader* table = amd64::early_boot_phys_to_virt(mm::phys_addr(*tablePhysAddress)).as_ptr<SDTHeader>();
#endif
                if(startsWith(table -> signature, ACPISignature<T>::value) && verifyTableChecksum(table) == ACPIChecksumResult::PASS){
                    tableCount++;
                }
            }
            Vector<T*> out(tableCount);
            for(uint64_t* tablePhysAddress = &(root -> tablePointer); (uint64_t)tablePhysAddress <
            (uint64_t)root + (root -> h.length); tablePhysAddress++){
#ifdef __x86_64__
                SDTHeader* table = amd64::early_boot_phys_to_virt(mm::phys_addr(*tablePhysAddress)).as_ptr<SDTHeader>();
#endif
                if(startsWith(table -> signature, ACPISignature<T>::value) && verifyTableChecksum(table) == ACPIChecksumResult::PASS){
                    out.push((T*)table);
                }
            }
            return out;
        }
        assertNotReached("RSDT signature did not match");
    }
}
#pragma GCC diagnostic pop
#endif //CROCOS_ACPI_H
