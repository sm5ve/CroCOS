//
// Created by Spencer Martin on 3/9/25.
//

#ifndef CROCOS_PAGETABLEMANAGER_H
#define CROCOS_PAGETABLEMANAGER_H

#include "stddef.h"
#include "stdint.h"

namespace kernel::amd64::PageTableManager{
    void init(size_t processorCount);
}

#endif //CROCOS_PAGETABLEMANAGER_H
