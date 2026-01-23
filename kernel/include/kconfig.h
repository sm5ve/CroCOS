//
// Created by Spencer Martin on 2/13/25.
//

#ifndef CROCOS_KCONFIG_H
#define CROCOS_KCONFIG_H

#ifndef HOSTED
#include <kbase.h>
#endif

#define SMP_TRAMPOLINE_START 8 // * 0x1000 bytes
#define KERNEL_STACK_SIZE (128 * 1024)
#define KERNEL_INIT_HEAP_BUFFER (512 * 1024) //A small initial buffer for the heap
#define KERNEL_INIT_LOG_LEVEL init::LoggingImportance::ERROR

#endif //CROCOS_KCONFIG_H
