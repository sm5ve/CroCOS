//
// ArchMocks.h - Testing utilities for arch.h mock implementations
// Created by Spencer Martin on 2/12/26.
//

#ifndef CROCOS_ARCHMOCKS_H
#define CROCOS_ARCHMOCKS_H

#include <cstddef>
#include <arch.h>

namespace arch {
#ifdef CROCOS_TESTING
    namespace testing {
        // ── The logical-CPU model ───────────────────────────────────────────
        //
        // A thread does not "get given" a CPU here; it declares which one it is
        // running as, and holds it exclusively for a scope. That is the property
        // the kernel actually relies on: every per-CPU structure (LocalPool's
        // paPage1/paPage2, CpuLocal storage, the RCU reader slots) assumes at
        // most one execution context per logical CPU at a time, and several of
        // them are plain non-atomic fields that are check-then-dereferenced.
        //
        // Binding is therefore explicit and exclusive, and getCurrentProcessorID()
        // asserts rather than inventing an answer for a thread that never bound.
        // An earlier mock assigned IDs implicitly (round-robin, modulo a CPU
        // count the fixture never set), which produced two live threads on one
        // logical CPU roughly 170 times per suite run and, when the count and
        // the pool count disagreed, an out-of-range LocalPool index and a SEGV.
        //
        // The harness binds each test's body thread to CPU 0 before the body
        // runs, so a single-threaded test needs no ceremony. A test that spawns
        // workers gives each one a ProcessorBinding, and releases the driver's
        // own binding for the duration with DetachedDriver — the driver is
        // parked in join() and genuinely occupies no CPU there.

        // Binds the calling thread to `id` for the lifetime of the guard, then
        // restores whatever binding it had before. Asserts that `id` is in range
        // and that no other live thread currently holds it.
        class ProcessorBinding {
        public:
            explicit ProcessorBinding(ProcessorID id);
            ~ProcessorBinding();
            ProcessorBinding(const ProcessorBinding&) = delete;
            ProcessorBinding& operator=(const ProcessorBinding&) = delete;
        private:
            ProcessorID previous;
            bool hadPrevious;
        };

        // Releases the calling thread's binding for the lifetime of the guard.
        // Use around a section where the thread spawns workers and then blocks:
        // it holds no CPU while parked, so a worker may legitimately take the
        // one it was using.
        class DetachedDriver {
        public:
            DetachedDriver();
            ~DetachedDriver();
            DetachedDriver(const DetachedDriver&) = delete;
            DetachedDriver& operator=(const DetachedDriver&) = delete;
        private:
            ProcessorID previous;
            bool hadPrevious;
        };

        // Bind the calling thread with no scope. Prefer ProcessorBinding; this
        // exists for the harness prologue, which has no scope to hang a guard on.
        void bindCurrentThread(ProcessorID id);
        void unbindCurrentThread();

        // Reset all per-CPU binding state between tests.
        void resetProcessorState();

        // Set the mock processor count for tests. Default is 8 if not set.
        // Asserts that no thread is currently bound outside the new range.
        void setProcessorCount(size_t count);

        // Get the current mock processor count
        size_t getProcessorCount();
    }
#endif
}

#endif // CROCOS_ARCHMOCKS_H
