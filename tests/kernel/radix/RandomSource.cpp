#include "RandomSource.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <chrono>

namespace CroCOSTest::radix {

uint64_t runSeed() {
    // Magic static: resolved once, thread-safe, and the print happens exactly
    // once no matter which thread asks first.
    static const uint64_t seed = [] {
        uint64_t s;
        const char* env = std::getenv("CROCOS_RADIX_SEED");
        if (env && *env) {
            s = std::strtoull(env, nullptr, 0);
            std::fprintf(stderr, "[radix] run seed %llu (from CROCOS_RADIX_SEED)\n",
                         static_cast<unsigned long long>(s));
        } else {
            s = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            // Printed unconditionally, not only on failure — see RandomSource.h.
            std::fprintf(stderr, "[radix] run seed %llu "
                                 "(replay with CROCOS_RADIX_SEED=%llu)\n",
                         static_cast<unsigned long long>(s),
                         static_cast<unsigned long long>(s));
        }
        std::fflush(stderr);
        return s;
    }();
    return seed;
}

}  // namespace CroCOSTest::radix
