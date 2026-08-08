//
// kernel::random — the placeholder entropy source (radix DEC-063).
//
// The seeding is the part that needs a kernel and the part a replacement will
// change, so it is not what is tested here. What IS tested is the mixing step,
// because it is pure, because RadixVM's placement leans on its output being
// uniform over a modulus, and because a mixer that is subtly wrong produces
// placements that are subtly clustered — which looks like bad luck rather than
// like a bug.
//
// These are sanity properties, not a statistical certification. splitmix64's
// real credentials are that it passes BigCrush, which is not something a unit
// test establishes. What a unit test can catch is the transcription error: a
// wrong constant, a wrong shift, a state update that does not advance.
//

#include "../test.h"
#include <TestHarness.h>

#include <Random.h>

#include <stdint.h>

using namespace CroCOSTest;
namespace rnd = kernel::random;

// ─── The stream advances, and does not repeat itself trivially ─────────────

TEST(random_splitmix64_advances_its_state) {
    uint64_t s = 1;
    const uint64_t a = rnd::splitmix64(s);
    const uint64_t b = rnd::splitmix64(s);
    const uint64_t c = rnd::splitmix64(s);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(b != c);
    ASSERT_TRUE(a != c);
    // The state itself moved — a mixer that returned a good-looking value while
    // leaving the state alone would pass a "successive draws differ" test only
    // by accident of the first one.
    ASSERT_TRUE(s != 1);
}

// A zero seed is the sentinel `next()` uses for "unseeded", and it must still
// produce a usable stream if it ever reaches the mixer — the sentinel is about
// the SLOT, not about zero being a bad state for splitmix64.
TEST(random_splitmix64_handles_a_zero_state) {
    uint64_t s = 0;
    const uint64_t a = rnd::splitmix64(s);
    const uint64_t b = rnd::splitmix64(s);
    ASSERT_TRUE(a != 0);
    ASSERT_TRUE(a != b);
}

// ─── Uniformity, at the resolution placement actually uses ─────────────────

// Placement takes `entropy() % positions` to pick a granule. What matters is
// that the low bits are not degenerate — a mixer whose output is, say, always
// even would halve the reachable placements and show up as an address space
// that never uses odd granules. Bucketed rather than bit-tested, because that
// is the property the consumer has.
TEST(random_splitmix64_is_not_degenerate_over_a_small_modulus) {
    constexpr unsigned kBuckets = 16;
    constexpr unsigned kDraws   = 64000;
    unsigned counts[kBuckets] = {};

    uint64_t s = 0x243F6A8885A308D3ull;
    for (unsigned i = 0; i < kDraws; i++) counts[rnd::splitmix64(s) % kBuckets]++;

    // Every bucket reached, and none wildly over-represented. The bounds are
    // deliberately loose — this is a smoke test for a transcription error, not
    // a chi-squared test, and a tight bound here would itself become flaky.
    const unsigned expected = kDraws / kBuckets;
    for (unsigned b = 0; b < kBuckets; b++) {
        ASSERT_TRUE(counts[b] > expected / 2);
        ASSERT_TRUE(counts[b] < expected * 2);
    }
}

// Two CPUs seeding within the same cycle — exactly what an SMP bringup does —
// must not share a stream. `next()` mixes the CPU id in for this reason; the
// property is checked here at the mixer level, since the seeding itself needs a
// kernel.
TEST(random_streams_from_adjacent_seeds_diverge) {
    uint64_t a = (0ull ^ (0x9E3779B97F4A7C15ull * 1)) | 1;   // as next() seeds cpu 0
    uint64_t b = (0ull ^ (0x9E3779B97F4A7C15ull * 2)) | 1;   // ...and cpu 1
    ASSERT_TRUE(a != b);

    unsigned collisions = 0;
    for (unsigned i = 0; i < 1000; i++) {
        if (rnd::splitmix64(a) == rnd::splitmix64(b)) collisions++;
    }
    // Independent 64-bit streams collide with probability ~1000 * 2^-64.
    ASSERT_EQ(unsigned{0}, collisions);
}
