//
// vmsmalloc Phase 8 — multi-threaded scenarios (TSan-validated).
//
// Each std::thread models one CroCOS CPU and binds to a distinct logical CPU
// (so each owns a private magazine, per DEC-014/030). Threads sharing a NUMA
// domain contend on that domain's lock-free partial[d][c] Treiber stack via
// flush (pushChain) and refill (pop) — the concurrency the harness exists to
// exercise. The release gate is TSan-clean on ARMv8 (M1); the ASan/leak runner
// runs the same scenarios for allocation-hygiene coverage.
//
// All multi-thread tests use TEST_WITH_TIMEOUT_NO_TRACKING so the harness's
// per-test leak tracker doesn't attribute std::thread machinery to the test.
//

#include "../../test.h"
#include <TestHarness.h>

#include <stddef.h>
#include <stdint.h>
#include <arch.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <VMSubstrateSlab.h>   // real
#include "mocks/MockCpuLocal.h"
#include "DebugIntrospection.h"

#include <thread>
#include <vector>
#include <atomic>
#include <unordered_set>

using namespace CroCOSTest;
namespace vms = kernel::mm::vmsmalloc;
namespace VS  = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;

namespace {

numa::DomainID allToZero(arch::ProcessorID)    { return numa::DomainID{0}; }
numa::DomainID cpuIsDomain(arch::ProcessorID i){ return numa::DomainID{static_cast<uint16_t>(i)}; }
numa::DomainID cpuMod2Domain(arch::ProcessorID i){ return numa::DomainID{static_cast<uint16_t>(i % 2)}; }

struct Harness {
    Harness(size_t cpus, size_t domains, numa::NUMAPolicy::Mapping mapping) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, mapping);
    }
    ~Harness() { VS::test::shutdown(); }
};

}  // namespace

// 11. N threads (distinct CPUs, one domain) each allocate M slots of one class;
//     every returned pointer is unique (no slot handed to two CPUs).
TEST_WITH_TIMEOUT_NO_TRACKING(vmsmalloc_concurrent_alloc_unique_single_domain, 10000) {
    constexpr size_t N = 6, M = 2000;
    const size_t c = 1;  // 16 B
    Harness h(N, 1, &allToZero);

    std::vector<std::vector<void*>> perThread(N);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < N; t++) {
        threads.emplace_back([t, c, &perThread] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(t));
            perThread[t].reserve(M);
            for (size_t k = 0; k < M; k++)
                perThread[t].push_back(VS::vmsmalloc(vms::slotSize(c)));
        });
    }
    for (auto& th : threads) th.join();

    std::unordered_set<void*> all;
    for (auto& v : perThread) for (void* p : v) all.insert(p);
    ASSERT_EQ(N * M, all.size());                         // all distinct

    kernel::test::bindThreadToCpu(0);                     // free from a domain-0 CPU
    for (auto& v : perThread) for (void* p : v) VS::vmsfree(p);
}

// 12. N threads each run alloc/free pairs on their own CPU; threads in the same
//     domain churn the shared stack via flush/refill. Balanced: every alloc is
//     freed by the same thread. Pass = no crash, no corruption (TSan/ASan clean).
TEST_WITH_TIMEOUT_NO_TRACKING(vmsmalloc_concurrent_alloc_free_balanced, 10000) {
    constexpr size_t N = 6, ITERS = 20000;
    Harness h(N, 1, &allToZero);
    std::atomic<uint64_t> ops{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < N; t++) {
        threads.emplace_back([t, &ops] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(t));
            uint64_t local = 0;
            for (size_t k = 0; k < ITERS; k++) {
                const size_t c = k % (vms::kNumSizeClasses - 1);   // skip whole-page class
                void* p = VS::vmsmalloc(vms::slotSize(c));
                // Touch the slot to let ASan/TSan catch bad addresses / races.
                *static_cast<volatile uint8_t*>(p) = static_cast<uint8_t>(t);
                VS::vmsfree(p);
                local++;
            }
            ops.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(uint64_t(N * ITERS), ops.load());
}

// 13. Cross-domain free: a producer on domain 0 allocates; a consumer on
//     domain 1 frees. The slabs must route to domain 0's shared stack; the
//     consumer's own magazine stays empty.
TEST_WITH_TIMEOUT_NO_TRACKING(vmsmalloc_concurrent_cross_domain_free, 10000) {
    constexpr size_t COUNT = 4000;
    const size_t c = 1;  // 16 B
    Harness h(2, 2, &cpuIsDomain);

    std::vector<void*> ptrs(COUNT);
    std::atomic<size_t> produced{0};
    std::atomic<bool> doneProducing{false};

    std::thread producer([&] {
        kernel::test::bindThreadToCpu(0);                 // domain 0
        for (size_t k = 0; k < COUNT; k++) {
            ptrs[k] = VS::vmsmalloc(vms::slotSize(c));
            produced.fetch_add(1, std::memory_order_release);
        }
        doneProducing.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        kernel::test::bindThreadToCpu(1);                 // domain 1
        size_t freed = 0;
        while (freed < COUNT) {
            size_t avail = produced.load(std::memory_order_acquire);
            while (freed < avail) VS::vmsfree(ptrs[freed++]);
        }
        // The cross-domain freer never touched its own magazine.
        ASSERT_EQ(0u, VS::test::magazineSnapshot(c).depth);
    });
    producer.join();
    consumer.join();

    // Everything routed to domain 0's shared stack (or was eager-freed); the
    // structure is intact and re-allocatable from domain 0.
    kernel::test::bindThreadToCpu(0);
    void* reuse = VS::vmsmalloc(vms::slotSize(c));
    ASSERT_TRUE(reuse != nullptr);
    VS::vmsfree(reuse);
}

// 14. Two same-domain CPUs hammer flush/refill with a tiny maxChainLength,
//     maximizing pushChain/pop CAS collisions on the shared stack.
TEST_WITH_TIMEOUT_NO_TRACKING(vmsmalloc_concurrent_flush_collision, 10000) {
    constexpr size_t N = 2, ITERS = 30000;
    const size_t c = vms::kNumSizeClasses - 1;  // 512 B (few slots -> frequent slab churn)
    Harness h(N, 1, &allToZero);
    VS::test::setMaxChainLength(numa::DomainID{0}, c, 2);

    std::vector<std::thread> threads;
    for (size_t t = 0; t < N; t++) {
        threads.emplace_back([t, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(t));
            // Hold a small working set so slabs flip Full/Partial repeatedly.
            void* held[4] = {nullptr, nullptr, nullptr, nullptr};
            for (size_t k = 0; k < ITERS; k++) {
                size_t slot = k & 3;
                if (held[slot]) VS::vmsfree(held[slot]);
                held[slot] = VS::vmsmalloc(vms::slotSize(c));
            }
            for (void* p : held) if (p) VS::vmsfree(p);
        });
    }
    for (auto& th : threads) th.join();
}

// 19. Sustained balanced stress across all slab classes on several CPUs.
//     Each thread frees everything it allocates; pass = clean under both
//     sanitizers with no corruption.
TEST_WITH_TIMEOUT_NO_TRACKING(vmsmalloc_stress_balanced_all_classes, 20000) {
    constexpr size_t N = 4, ITERS = 50000, WINDOW = 16;
    Harness h(N, 2, &cpuMod2Domain);   // cpus 0,2 -> dom0; 1,3 -> dom1 (cross-domain frees)

    std::vector<std::thread> threads;
    for (size_t t = 0; t < N; t++) {
        threads.emplace_back([t] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(t));
            void* window[WINDOW] = {};
            uint32_t rng = 0x1234567u ^ static_cast<uint32_t>(t * 2654435761u);
            for (size_t k = 0; k < ITERS; k++) {
                rng = rng * 1664525u + 1013904223u;       // LCG
                size_t idx = (rng >> 8) % WINDOW;
                if (window[idx]) {
                    VS::vmsfree(window[idx]);
                    window[idx] = nullptr;
                } else {
                    size_t c = (rng >> 16) % (vms::kNumSizeClasses - 1);
                    window[idx] = VS::vmsmalloc(vms::slotSize(c));
                    *static_cast<volatile uint8_t*>(window[idx]) = 0xA5;
                }
            }
            for (void* p : window) if (p) VS::vmsfree(p);
        });
    }
    for (auto& th : threads) th.join();
}
