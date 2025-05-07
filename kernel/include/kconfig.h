//
// Created by Spencer Martin on 2/13/25.
//

#ifndef CROCOS_KCONFIG_H
#define CROCOS_KCONFIG_H

#define VMEM_OFFSET 0xffffffff80000000
#define KERNEL_STACK_SIZE (64 * 1024)
#define KERNEL_BUMP_ALLOC_SIZE (8 * 1024) // a small buffer for the bump allocator before we have a proper heap

#endif //CROCOS_KCONFIG_H
