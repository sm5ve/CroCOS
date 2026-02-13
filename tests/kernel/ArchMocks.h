//
// ArchMocks.h - Testing utilities for arch.h mock implementations
// Created by Spencer Martin on 2/12/26.
//

#ifndef CROCOS_ARCHMOCKS_H
#define CROCOS_ARCHMOCKS_H

#include <cstddef>

namespace arch {
#ifdef CROCOS_TESTING
    namespace testing {
        // Reset all processor ID mappings between tests
        // This clears the thread->processor mapping and resets the counter
        void resetProcessorState();

        // Set the mock processor count for tests
        // Default is 8 if not set
        void setProcessorCount(size_t count);

        // Get the current mock processor count
        size_t getProcessorCount();
    }
#endif
}

#endif // CROCOS_ARCHMOCKS_H
