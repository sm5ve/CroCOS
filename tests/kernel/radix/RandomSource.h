//
// radix-tree Phase 0 — the harness's seedable entropy source (§12 requirement 2).
//
// "A seedable, deterministic entropy source under test, or probe failures will
// not reproduce." Everything random in this harness draws from here: randomized
// operation sequences, backoff jitter, and (from Phase 3) probe placement.
//
// This is the TEST side only. The in-kernel source is separate, out-of-spec
// prerequisite work (DEC-063) and is deliberately not modelled here — DEC-063's
// own rationale notes that a non-cryptographic generator is exactly right for
// the harness, where determinism is the point, and exactly wrong for the kernel,
// where DEC-008's base randomisation is a security property.
//
// Two rules the shape enforces:
//
//   - **The run seed is printed on every run**, whether or not anything fails.
//     A seed printed only on failure is useless for the case that matters: a
//     rare interleaving that passed 99 times and failed once, where you want to
//     re-run the *passing* seeds to bisect.
//   - **Each stream is seeded independently from (runSeed, streamId)**, not by
//     sharing one generator. Sharing makes a per-CPU thread's sequence depend on
//     how the other threads interleaved, so a "reproducible" seed reproduces
//     nothing under concurrency — the exact failure this file exists to avoid.
//

#ifndef CROCOS_RADIX_RANDOM_SOURCE_H
#define CROCOS_RADIX_RANDOM_SOURCE_H

#include <stdint.h>
#include <stddef.h>

namespace CroCOSTest::radix {

    // splitmix64 — used both to derive stream seeds and as the generator. Cheap,
    // well-distributed, and (the property that matters here) exactly
    // reproducible across platforms, which a std::mt19937 seeded by
    // std::random_device is not.
    class RandomSource {
    public:
        RandomSource() : state(0) {}
        explicit RandomSource(uint64_t seed) : state(seed) {}

        uint64_t next() {
            state += 0x9E3779B97F4A7C15ull;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        // Uniform in [0, bound). Rejection-sampled rather than modulo-biased:
        // a placement test that probes a small bound would otherwise skew, and
        // a skewed probe distribution silently weakens exactly the coverage the
        // probe tests are measuring.
        uint64_t below(uint64_t bound) {
            if (bound <= 1) return 0;
            const uint64_t limit = UINT64_MAX - (UINT64_MAX % bound) - 1;
            uint64_t r;
            do { r = next(); } while (r > limit);
            return r % bound;
        }

        uint64_t inRange(uint64_t lo, uint64_t hi) {   // [lo, hi]
            return lo + below(hi - lo + 1);
        }

        bool chance(unsigned percent) { return below(100) < percent; }

    private:
        uint64_t state;
    };

    // The run seed. Taken from the CROCOS_RADIX_SEED environment variable when
    // set (that is how you replay a failure), otherwise drawn once per process
    // and printed.
    uint64_t runSeed();

    // A generator for one logical stream — one per test, or one per CPU thread
    // within a test. streamId must be distinct per concurrent user.
    inline RandomSource streamFor(uint64_t streamId) {
        RandomSource mixer(runSeed() ^ (streamId * 0xD1B54A32D192ED03ull));
        return RandomSource(mixer.next());
    }

}  // namespace CroCOSTest::radix

#endif  // CROCOS_RADIX_RANDOM_SOURCE_H
