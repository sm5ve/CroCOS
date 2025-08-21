//
// Created by Spencer Martin on 8/1/25.
//
#include <liballoc/Backend.h>
#include <sys/mman.h>

namespace LibAlloc::Backend{
#ifdef __x86_64__
    const size_t smallPageSize = 4096;
    const size_t largePageSize = smallPageSize * 512;
#else
    #ifdef __APPLE__
        #ifdef __aarch64__
    const size_t smallPageSize = 16 * 1024;
    const size_t largePageSize = 2 * 1024 * 1024;
        #else
            #error "Unsupported architecture"
        #endif
    #endif
#endif

    void* allocPages(size_t count){
        return mmap(nullptr, count * smallPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    //The caller is responsible for retaining information on page counts of allocations
    void freePages(void* ptr, size_t count){
        munmap(ptr, count * smallPageSize);
    }
}