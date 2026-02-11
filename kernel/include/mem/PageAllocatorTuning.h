//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATORTUNING_H
#define CROCOS_PAGEALLOCATORTUNING_H

constexpr size_t LOCAL_POOL_FREE_COMFORT_THRESHOLD = 16;
constexpr size_t MAX_BATCH_SIZE = 32;
constexpr size_t LOCK_RETRY_COUNT = 4;
constexpr size_t LOCK_DELAY_ITERATIONS = 100;
constexpr size_t MAX_COLOR_COUNT = 0x20;

#endif //CROCOS_PAGEALLOCATORTUNING_H