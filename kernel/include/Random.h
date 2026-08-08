//
// kernel::random — the placeholder entropy source (radix DEC-063).
//
// ─────────────────────────────────────────────────────────────────────────
//  THIS IS NOT A CSPRNG. IT IS NOT SUITABLE FOR ASLR AGAINST AN ATTACKER.
// ─────────────────────────────────────────────────────────────────────────
//
// DEC-063 names a real in-kernel entropy source as out-of-spec prerequisite
// work, and it has not been done. This exists so the consumers that need *a*
// source — RadixVM's placement probing and its guard-gap sizing — can be built
// and tested now, and it is deliberately shaped so that replacing it is a
// one-file change rather than an audit.
//
// **What it is**: splitmix64 over per-CPU state, lazily seeded on first use from
// a cycle counter mixed with the CPU id. Statistically fine — splitmix64 passes
// BigCrush — and completely predictable to anyone who can observe or guess the
// seed. A cycle counter at boot is low-entropy and partially attacker-influenced
// on a machine they can time.
//
// **What a real one owes** (the checklist for whoever rips this out):
//
//   1. A cryptographic construction — ChaCha20 or equivalent — not a
//      non-cryptographic mixer, however good its statistics.
//   2. Seeding from a hardware source (RDSEED/RDRAND on amd64) with an explicit
//      answer for what happens when it is absent or fails. "Fall back to the
//      cycle counter" is a policy decision, not a default.
//   3. A reseeding policy, and a defined behaviour for callers before the first
//      reseed.
//   4. A decision about whether ASLR-grade and non-ASLR-grade callers share one
//      source. They should probably not: `CoreTree`'s backoff jitter needs
//      decorrelation and nothing more, and is deliberately NOT routed here.
//
// **How to rip it out**: replace this header and `kernel/Random.cpp`, keeping
// `next()` and `Entropy`. Nothing else in the kernel names anything from this
// file — placement takes its entropy as a template parameter precisely so that
// the quality requirement can differ per consumer without a rewrite.
//

#ifndef CROCOS_RANDOM_H
#define CROCOS_RANDOM_H

#include <stdint.h>

namespace kernel::random {

    // A 64-bit value from this CPU's stream. Lock-free and contention-free —
    // the state is per-CPU, so this is a load, three multiplies and a store,
    // with no atomics at all.
    //
    // Callable from any context after `arch::getCurrentProcessorID()` works.
    // There is no init to call: the first draw on a CPU seeds that CPU's slot,
    // which keeps this out of the boot ordering entirely and is one less thing
    // for a replacement to preserve.
    [[nodiscard]] uint64_t next();

    // The functor shape RadixVM's placement wants. A type rather than a
    // function pointer so the call inlines and so a test can substitute a
    // seedable source without either side knowing.
    struct Entropy {
        [[nodiscard]] uint64_t operator()() const { return next(); }
    };

    // The mixing step, exposed for testing only. Pure, so its distribution
    // properties can be checked in userspace without a kernel — the seeding is
    // the part that needs a kernel, and the part a replacement will change.
    [[nodiscard]] constexpr uint64_t splitmix64(uint64_t& state) {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

}  // namespace kernel::random

#endif  // CROCOS_RANDOM_H
