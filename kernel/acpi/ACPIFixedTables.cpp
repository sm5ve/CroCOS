//
// Created by Spencer Martin on 2/16/25.
//

#include <acpi.h>
#include <kconfig.h>
#include <kernel.h>
#include <lib/ds/Vector.h>
#include <lib/str.h>

#ifdef __x86_64__
#include <arch/amd64.h>
#endif

namespace kernel::acpi{
    struct SDTHeader* rsdt = nullptr;

    ACPIChecksumResult verifyTableChecksum(SDTHeader* header){
        uint8_t checksum = 0;
        uint8_t* bytes = (uint8_t*)header;
        for(size_t i = 0; i < header -> length; i++){
            checksum += *bytes;
            bytes++;
        }

        if(checksum){
            return ACPIChecksumResult::FAIL;
        }
        return ACPIChecksumResult::PASS;
    }

    ACPIDiscoveryResult tryFindACPI(){
        struct RSDP* rsdp = nullptr;
#ifdef __x86_64__
        //FIXME: this is a very naive way of finding the RDSP
        for(char* ptr = amd64::early_boot_phys_to_virt(mm::phys_addr((uint64_t)0)).as_ptr<char>();
        ptr < amd64::early_boot_phys_to_virt(mm::phys_addr((uint64_t)0x100000)).as_ptr<char>(); ptr += 16){
            if(startsWith(ptr, "RSD PTR ")){
                rsdp = (RSDP*)ptr;
            }
        }
#endif
        //If we didn't find the rsdp, bail
        if(rsdp == nullptr){
            return ACPIDiscoveryResult::NOT_FOUND;
        }

        uint8_t checksum = 0;
        uint8_t* checksumValidationPtr = (uint8_t*)rsdp;
        for(auto i = 0; i < 20; i++){ //Size of first part of rsdp
            checksum += *checksumValidationPtr;
            checksumValidationPtr++;
        }

        if(checksum != 0){ //The rsdp has 2 checksums - here we verify the first.
            //Do we want to try to find another rsdp if the checksum fails? Like if for some reason something
            //in the lower megabyte of memory as the signature "RSD PTR " but isn't actually the rsdp...
            return ACPIDiscoveryResult::CHECKSUM_FAIL;
        }

        if(rsdp -> revision == 2){
            //If revision == 2, then this is an rsdp rather than an RSDP. We need to perform
            //A checksum on the additional parts too.
            for(auto i = 0; i < 12; i++){ //Size of second part of rsdp
                checksum += *checksumValidationPtr;
                checksumValidationPtr++;
            }

            if(checksum != 0){
                return ACPIDiscoveryResult::CHECKSUM_FAIL;
            }
        }

        if(rsdp -> revision == 0){
#ifdef __x86_64__
            rsdt = amd64::early_boot_phys_to_virt(mm::phys_addr(rsdp -> rsdtAddress)).as_ptr<SDTHeader>();
#endif
        }
        else{
#ifdef __x86_64__
            rsdt = amd64::early_boot_phys_to_virt(mm::phys_addr(rsdp -> xsdtAddress)).as_ptr<SDTHeader>();
#endif
        }

        if(verifyTableChecksum(rsdt) == ACPIChecksumResult::FAIL){
            return ACPIDiscoveryResult::CHECKSUM_FAIL;
        }

        if(!startsWith(rsdt -> signature, "RSDT") && !startsWith(rsdt -> signature, "XSDT")){
            return ACPIDiscoveryResult::MISMATCHED_SIGNATURE;
        }

        return ACPIDiscoveryResult::SUCCESS;
    }

    size_t MADT::getEnabledProcessorCount() {
        size_t procCount = 0;
        for(acpi::MADTEntryHeader* header = &(this->tableEntries);
        (uint64_t)header < (uint64_t)this + this -> h.length;
        header = (acpi::MADTEntryHeader*)((uint64_t)header + header -> length)){
            if(header -> type == 0){
                MADT_LAPIC_Entry* entry = (MADT_LAPIC_Entry*)header;
                //TODO handle processors that are not yet enabled but still present
                if((entry -> flags & 3) == 1){
                    procCount++;
                }
            }
        }
        return procCount;
    }
}