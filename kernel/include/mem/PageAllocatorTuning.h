//
// Created by Spencer Martin on 2/5/26.
//

#ifndef CROCOS_PAGEALLOCATORTUNING_H
#define CROCOS_PAGEALLOCATORTUNING_H

constexpr size_t LOCAL_POOL_FREE_COMFORT_THRESHOLD = 16;
constexpr size_t MAX_ALLOC_BATCH_SIZE = 32;
constexpr size_t MAX_FREE_BATCH_SIZE = 32;
constexpr size_t LOCK_RETRY_COUNT = 4;
constexpr size_t LOCK_DELAY_ITERATIONS = 100;
constexpr size_t MAX_COLOR_COUNT = 0x20;

constexpr size_t SMALL_PAGE_WEIGHT_NUM = 1;
constexpr size_t SMALL_PAGE_WEIGHT_DEN = 2;

constexpr size_t MODERATE_THRESHOLD_MINIMUM = 4;



#endif //CROCOS_PAGEALLOCATORTUNING_H