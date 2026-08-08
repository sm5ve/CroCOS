//
// kernel::random — the placeholder entropy source (radix DEC-063).
//
// See Random.h for what this is not. The whole implementation is here so that
// replacing it means replacing two files and nothing else.
//

#include <Random.h>

#include <arch.h>
#include <kassert.h>

namespace kernel::random {

namespace {

    // Per-CPU state, cache-line separated. False sharing here would be
    // self-inflicted: the whole reason the state is per-CPU is to avoid the
    // contention an atomic global would have, and packing sixteen CPUs' states
    // into one line reintroduces it in a form that does not show up as a lock.
    struct alignas(arch::CACHE_LINE_SIZE) CpuState {
        uint64_t state;
        // Zero is the "unseeded" sentinel, which costs one comparison per draw
        // and buys not having to appear in the boot sequence at all. A seeded
        // state can legitimately BE zero after a step, so the seeding writes a
        // value forced non-zero rather than trusting the mixer.
        uint64_t padding[(arch::CACHE_LINE_SIZE / sizeof(uint64_t)) - 1];
    };

    CpuState gState[arch::MAX_PROCESSOR_COUNT] = {};

    // A cheap, monotonic-ish counter to seed from.
    //
    // This is the low-entropy step, and it is where a real implementation puts
    // RDSEED. Deliberately not dressed up: a boot-time cycle count is guessable
    // to within a wide but finite window by anyone who can time the machine.
    [[nodiscard]] uint64_t seedCounter() {
#if defined(__x86_64__)
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
        uint64_t v;
        asm volatile("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#else
        // No counter: the stream is then a pure function of the CPU id, which is
        // reproducible across boots. Correct for testing, useless for ASLR —
        // and loud here rather than silently degraded, because a port that
        // lands on this branch has NOT got an entropy source, whatever the
        // calls return.
#warning "kernel::random has no cycle counter on this architecture — the stream is deterministic across boots (radix DEC-063)"
        return 0;
#endif
    }

}  // namespace

uint64_t next() {
    const auto cpu = static_cast<size_t>(arch::getCurrentProcessorID());
    assert(cpu < arch::MAX_PROCESSOR_COUNT,
           "kernel::random: processor id beyond the compile-time cap");
    CpuState& s = gState[cpu];

    if (s.state == 0) {
        // Mix the CPU id in so two CPUs seeding within the same cycle — which
        // is exactly what an SMP bringup does — do not produce identical
        // streams. Forced non-zero so the sentinel cannot be re-armed by a
        // seed that happens to be zero.
        s.state = (seedCounter() ^ (0x9E3779B97F4A7C15ull * (cpu + 1))) | 1;
    }
    return splitmix64(s.state);
}

}  // namespace kernel::random
