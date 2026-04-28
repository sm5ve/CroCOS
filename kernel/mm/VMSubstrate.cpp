//
// Created by Spencer Martin on 4/26/26.
//

#include <mem/VMSubstrate.h>
#include <mem/mm.h>
#include <arch.h>

#include <kmemlayout.h>

namespace kernel::mm::VMSubstrate {

    arch::PageTable<pageTableLevelForKMemRegion() - 1> vmmAreaTable;



    bool init() {
        return true;
    }
}
