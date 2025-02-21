//
// Created by Spencer Martin on 2/17/25.
//

#include <mm.h>
#include <kconfig.h>

#include <kernel.h>

#define early_boot_phys_to_virt(x) ((void*)((uint64_t)x + VMEM_OFFSET))
#define early_boot_virt_to_phys(x) ((void*)((uint64_t)x - VMEM_OFFSET))

bool inEarlyBoot = true;

namespace kernel::mm{
    phys_addr virt_to_phys(virt_addr vaddr){
        if(inEarlyBoot){
            if(vaddr.value >= VMEM_OFFSET){
                return phys_addr(early_boot_virt_to_phys(vaddr.value));
            }
            else{
                return phys_addr(nullptr); //TODO switch this to return an optional, return None here
            }
        }
        return phys_addr(nullptr); //TODO switch this to return an optional, return None here
    }

    virt_addr phys_to_virt(phys_addr paddr){
        if(inEarlyBoot){
            if(~(paddr.value) >= VMEM_OFFSET){
                return virt_addr(early_boot_phys_to_virt(paddr.value));
            }
            else{
                return virt_addr(nullptr); //TODO switch this to return an optional, return None here
            }
        }
        return virt_addr(nullptr); //TODO switch this to return an optional, return None here
    }

    size_t phys_memory_range::getSize() {
        return this -> end.value - this -> start.value;
    }
}