//
// Created by Spencer Martin on 2/15/25.
//

#ifndef CROCOS_ARCH_H
#define CROCOS_ARCH_H

#include "stdint.h"
#include "stddef.h"
#include <arch/PageTableSpecification.h>
#include <core/ds/Optional.h>

#ifndef ARCH_OVERRIDE
#ifdef __x86_64__
#define ARCH_AMD64
#endif
#endif

#ifdef ARCH_AMD64
#include "arch/amd64/amd64.h"
#endif

#include <arch/memmap.h>

#ifdef ARCH_AMD64
#define SUPPORTS_SPINLOCK_DEADLOCK_DETECTION
#endif

namespace arch{
    void serialOutputString(const char* str);

#ifdef CROCOS_TESTING
    void enableInterrupts();
    void disableInterrupts();
    bool areInterruptsEnabled();
    using ProcessorID = uint8_t;
    constexpr size_t MAX_PROCESSOR_COUNT = 256;
    constexpr size_t CACHE_LINE_SIZE = 64;
    struct InterruptFrame {};
#endif

#ifdef ARCH_AMD64
    using ProcessorID = amd64::ProcessorID;
    constexpr size_t MAX_PROCESSOR_COUNT = 256;
    constexpr size_t CACHE_LINE_SIZE = 64;
    using InterruptFrame = amd64::interrupts::InterruptFrame;
#ifndef CROCOS_TESTING
    constexpr auto enableInterrupts = amd64::sti;
    constexpr auto disableInterrupts = amd64::cli;
    constexpr auto areInterruptsEnabled = amd64::interrupts::areInterruptsEnabled;
#endif
    constexpr size_t CPU_INTERRUPT_COUNT = amd64::INTERRUPT_VECTOR_COUNT;
    constexpr auto pageTableDescriptor = amd64::pageTableDescriptor;
    inline void flushTLB() {
        amd64::flushTLB();
    }
    using MemMapIterator = amd64::MultibootMMapIterator;
    inline size_t debugEarlyBootCPUID() {
        return amd64::debugEarlyBootCPUID();
    }
#endif

#ifndef CROCOS_TESTING
    template <size_t level>
    using PTE = PageTableEntry<pageTableDescriptor.levels[level]>;
    template <size_t level>
    struct PageTable{
        PTE<level> data alignas(sizeof(PTE<level>) * pageTableDescriptor.entryCount[level]) [pageTableDescriptor.entryCount[level]];
        PTE<level>& operator[](size_t index){return data[index];}
        const PTE<level>& operator[](size_t index) const {return data[index];}
    } __attribute__((packed));

    constexpr size_t smallPageSize = 1ull << pageTableDescriptor.getVirtualAddressBitCount(pageTableDescriptor.LEVEL_COUNT); //4KiB
    constexpr size_t bigPageSize = 1ull << pageTableDescriptor.getVirtualAddressBitCount(pageTableDescriptor.LEVEL_COUNT - 1); //2MiB
    constexpr size_t maxMemorySupported = 1ull << 1ull << pageTableDescriptor.getVirtualAddressBitCount(); //256 TiB
#else
    constexpr size_t smallPageSize = 1ull << 12;
    constexpr size_t bigPageSize = 1ull << 21;
    constexpr size_t maxMemorySupported = 1ull << 48;
#endif
    //Guaranteed to be between 0 and (the total number of logical processors - 1)
    ProcessorID getCurrentProcessorID();
    size_t processorCount();
#ifndef CROCOS_TESTING
    static_assert(MemoryMapIterator<MemMapIterator>);

    IteratorRange<MemMapIterator> getMemoryMap();
#endif

    class SerialPrintStream : public Core::PrintStream{
    protected:
        void putString(const char*) override;
    };

    class InterruptDisabler {
    private:
        bool wasEnabled;
        bool active;
    public:
        InterruptDisabler();
        ~InterruptDisabler();

        void release();
    };

    enum class InterruptState : uint8_t {
        ENABLED,
        DISABLED,
        STALE
    };

    class InterruptResetter {
    private:
        InterruptState state;

        friend class InterruptDisablingSpinlock;
        friend class InterruptDisablingRWSpinlock;
        friend class InterruptDisablingPrioritySpinlock;

        explicit InterruptResetter(InterruptState s) : state(s) {}
    public:
        explicit InterruptResetter() : state(InterruptState::STALE) {}
        InterruptResetter(const InterruptResetter&) = delete;
        InterruptResetter& operator=(const InterruptResetter&) = delete;
        InterruptResetter(InterruptResetter&&) = default;
        InterruptResetter& operator=(InterruptResetter&&) = default;

        void operator()();
    };

    class InterruptDisablingSpinlock {
    private:
        InterruptState state;
        Atomic<bool> acquired;
        Atomic<size_t> metadata;
    public:
        InterruptDisablingSpinlock();

        void acquire(); //Acquire the lock, disable interrupts once acquired, store interrupt state
        void acquirePlain(); //Acquire the lock, leave interrupts as-is
        bool tryAcquire(); //Fallible version of acquire
        bool tryAcquirePlain(); //Fallible version of acquirePlain
        void release(); //Release the lock, revert interrupt flag to prior state
        InterruptResetter releasePlain(); //Release the lock, maintain interrupt flag
        [[nodiscard]] bool lock_taken() const;
    };

    class InterruptDisablingRWSpinlock {
    public:
        class ReaderLockGuard {
            InterruptState state;

            friend class InterruptDisablingRWSpinlock;

            ReaderLockGuard(InterruptState s) : state(s) {}
        public:
            ReaderLockGuard(const ReaderLockGuard&) = delete;
            ReaderLockGuard& operator=(const ReaderLockGuard&) = delete;
            ReaderLockGuard(ReaderLockGuard&&) = default;
            ReaderLockGuard& operator=(ReaderLockGuard&&) = default;
        };

        class WriterLockGuard {
            InterruptState state;

            friend class InterruptDisablingRWSpinlock;

            WriterLockGuard(InterruptState s) : state(s) {}
        public:
            WriterLockGuard(const WriterLockGuard&) = delete;
            WriterLockGuard& operator=(const WriterLockGuard&) = delete;
            WriterLockGuard(WriterLockGuard&&) = default;
            WriterLockGuard& operator=(WriterLockGuard&&) = default;
        };

        class InterruptResetter {
            InterruptState state;

            friend class InterruptDisablingRWSpinlock;

            InterruptResetter(InterruptState s) : state(s) {}
        public:
            InterruptResetter(const InterruptResetter&) = delete;
            InterruptResetter& operator=(const InterruptResetter&) = delete;
            InterruptResetter(InterruptResetter&&) = default;
            InterruptResetter& operator=(InterruptResetter&&) = default;

            void operator()();
        };

    private:
        Atomic<uint64_t> lockstate{0};
        static const size_t activeMeta = 1ul << 63;
        Atomic<size_t> metadata{0};

    public:
        InterruptDisablingRWSpinlock();

        ReaderLockGuard acquireReader();
        void acquireReaderPlain();
        Optional<ReaderLockGuard> tryAcquireReader();
        bool tryAcquireReaderPlain();
        void releaseReader(const ReaderLockGuard& guard);
        InterruptResetter releaseReaderPlain();

        WriterLockGuard acquireWriter();
        void acquireWriterPlain();
        Optional<WriterLockGuard> tryAcquireWriter();
        bool tryAcquireWriterPlain();
        void releaseWriter(const WriterLockGuard& guard);
        InterruptResetter releaseWriterPlain();

        [[nodiscard]] bool writerLockTaken() const;
        [[nodiscard]] bool readerLockTaken() const;
        [[nodiscard]] bool lockTaken() const;
    };

    class InterruptDisablingPrioritySpinlock {
        Atomic<uint8_t> lockstate{0};
        static const size_t activeMeta = 1ul << 63;
        Atomic<size_t> metadata{0};
        InterruptState priorityState{InterruptState::STALE};
        InterruptState normalState{InterruptState::STALE};

    public:
        InterruptDisablingPrioritySpinlock();

        void acquirePriority();
        void acquirePriorityPlain();
        bool tryAcquirePriority();
        bool tryAcquirePriorityPlain();
        void releasePriority();
        InterruptResetter releasePriorityPlain();

        void acquire();
        void acquirePlain();
        bool tryAcquire();
        bool tryAcquirePlain();
        void release();
        InterruptResetter releasePlain();

        [[nodiscard]] bool priorityLockTaken() const;
        [[nodiscard]] bool normalLockTaken() const;
        [[nodiscard]] bool lockTaken() const;
    };
#ifndef CROCOS_TESTING
    static_assert([] {
        for (const auto e : pageTableDescriptor.entryCount) {
            if (largestPowerOf2Dividing(e) != e) {
                return false;
            }
        }
       return true;
    }(), "Page tables must have a power-of-two number of entries");
#endif
}
#endif //CROCOS_ARCH_H
