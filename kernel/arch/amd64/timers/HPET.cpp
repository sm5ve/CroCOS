//
// Created by Spencer Martin on 12/30/25.
//
#include <acpi.h>
#include <timing.h>
#include <arch/amd64/timers/HPET.h>
#include <core/FrequencyData.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/hal/Clock.h>

#include <core/utility.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
namespace kernel::amd64::timers{
    constexpr uint32_t maximumClockPeriod = 0x05F5E100; //according to the osdev wiki

    struct HPETComparatorRegisters {
        volatile uint32_t configCapabilities;
        uint32_t interruptRouteCapabilities;
        volatile uint64_t comparatorValue;
        uint64_t interruptRoute;
        uint64_t reserved2;

        [[nodiscard]] bool supportsFSBRouting() const {
            return configCapabilities & (1 << 15);
        }

        void enableFSBRouting(bool shouldEnable = true) {
            if (shouldEnable) {
                assert(supportsFSBRouting(), "Tried to enable FSB routing on comparator that does not support it");
            }
            configCapabilities = (configCapabilities & ~(1u << 14)) | (shouldEnable ? (1 << 14) : 0);
        }

        [[nodiscard]] bool usingFSBRouting() const {
            return configCapabilities & (1 << 14);
        }

        void setIOAPICRouting(size_t pin) {
            auto pinMask = (1ul << pin);
            assert(pinMask & interruptRouteCapabilities, "Tried to route comparator to unsupported IOAPIC line");
            configCapabilities = (configCapabilities & ~(0x1fu << 9)) | static_cast<uint16_t>(pin << 9);
        }

        void set32BitCounter(bool shouldUse32Bit = true) {
            configCapabilities = (configCapabilities & ~(1u << 8)) | (shouldUse32Bit ? (1 << 8) : 0);
        }

        void setWritableAccumulator() {
            configCapabilities |= 1u << 6;
        }

        [[nodiscard]] bool using32BitCounter() const {
            return (configCapabilities & (1 << 8)) | !supports64BitCounter();
        }

        [[nodiscard]] bool supports64BitCounter() const {
            return configCapabilities & (1 << 5);
        }

        [[nodiscard]] bool supportsPeriodicMode() const {
            return configCapabilities & (1 << 4);
        }

        void setPeriodicMode(bool shouldUsePeriodic = true) {
            if (shouldUsePeriodic) {
                assert(supportsPeriodicMode(), "Tried to set periodic mode on comparator that does not support it");
            }
            configCapabilities = (configCapabilities & ~(1u << 3)) | (shouldUsePeriodic ? (1 << 3) : 0);
        }

        [[nodiscard]] bool isPeriodicMode() const {
            return configCapabilities & (1 << 3);
        }

        void enableInterrupt(bool shouldEnable = true) {
            configCapabilities = (configCapabilities & ~(1u << 2)) | (shouldEnable ? (1 << 2) : 0);
        }

        [[nodiscard]] bool isInterruptEnabled() const {
            return configCapabilities & (1 << 2);
        }

        void generateLevelTriggeredInterrupt(bool level = true) {
            configCapabilities = (configCapabilities & ~(1u << 1)) | (level ? (1 << 1) : 0);
        }

    } __attribute__((packed));

    struct HPETRegisters {
        //Starts at 0x00
        uint32_t deviceInfo;
        /*uint8_t revisionID;
        uint8_t capabilities;
        uint16_t vendorID;*/
        uint32_t clockPeriod;
        uint64_t rsv0;
        //Starts at 0x10
        uint64_t generalConfiguration;
        uint64_t rsv1;
        //Starts at 0x20
        volatile uint64_t interruptStatusRegister;
        //0x28
        uint8_t rsv2[0xf0 - 0x28];
        //Starts at 0xf0
        volatile uint64_t mainCounter;
        uint64_t rsv3;
        //Starts at 0x100
        HPETComparatorRegisters _comparators;

        [[nodiscard]] size_t comparatorCount() const{
            return ((deviceInfo >> 8) & 0x1f) + 1;
        }

        [[nodiscard]] bool longCountersSupported() const {
            return ((deviceInfo >> 8) & 0x20) != 0;
        }

        [[nodiscard]] bool legacyReplacementSupported() const {
            return ((deviceInfo >> 8) & 0x80) != 0;
        }

        [[nodiscard]] bool legacyReplacementEnabled() const {
            return generalConfiguration & 2;
        }

        [[nodiscard]] bool enabled() const {
            return generalConfiguration & 1;
        }

        void enableLegacyReplacementMode(bool enabled = true) {
            //Do I need memory fencing here?
            generalConfiguration = (generalConfiguration & ~2ul) | (enabled ? 2 : 0);
        }

        void enable(bool enabled = true) {
            generalConfiguration = (generalConfiguration & ~1ul) | (enabled ? 1 : 0);
        }

        [[nodiscard]] bool didTimerRaiseInterrupt(size_t comparatorIndex) const {
            return interruptStatusRegister & (1ul << comparatorIndex);
        }

        void acknowledgeTimerInterrupt(size_t timerIndex) {
            interruptStatusRegister |= (1ul << timerIndex);
        }

        [[nodiscard]] uint64_t getMainTimerValue() const {
            return mainCounter;
        }

        void setMainTimerValue(uint64_t value) {
            bool shouldEnable = enabled();
            enable(false);
            mainCounter = value;
            enable(shouldEnable);
        }

        [[nodiscard]] HPETComparatorRegisters& comparatorRegs(size_t index) {
            assert(index < comparatorCount(), "Invalid comparator index");
            return (&_comparators)[index];
        }

        IteratorRange<HPETComparatorRegisters*> comparators() {
            return {&_comparators, &_comparators + comparatorCount()};
        }
    } __attribute__((packed));

    constexpr uint64_t _getComparatorStartOffset() {
        HPETRegisters regs = {};
        uint64_t offset = reinterpret_cast<uint64_t>(&regs._comparators) - reinterpret_cast<uint64_t>(&regs);
        return offset;
    }

    constexpr uint64_t _getMainCounterOffset() {
        HPETRegisters regs = {};
        uint64_t offset = reinterpret_cast<uint64_t>(&regs.mainCounter) - reinterpret_cast<uint64_t>(&regs);
        return offset;
    }

    constexpr uint64_t _getISROffset() {
        HPETRegisters regs = {};
        uint64_t offset = reinterpret_cast<uint64_t>(&regs.interruptStatusRegister) - reinterpret_cast<uint64_t>(&regs);
        return offset;
    }

    static_assert(_getComparatorStartOffset() == 0x100);
    static_assert(_getMainCounterOffset() == 0xf0);
    static_assert(_getISROffset() == 0x20);
    static_assert(sizeof(HPETComparatorRegisters) == 0x20);

    HPETRegisters& mapHPET(acpi::HPET& hpetTable) {
        assert(hpetTable.hpetBaseAddress.addressSpaceID == acpi::GASAddressSpaceID::SYSTEM_MEMORY, "We expect the HPET to be mapped to system memory.");
        auto hpetBaseAddress = (void*)hpetTable.hpetBaseAddress.address;
        const auto mappedBase = PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(hpetBaseAddress));
        return *static_cast<HPETRegisters*>(mappedBase);
    }

    //I typically expect that the HPET will connect to all IOAPIC pins (or at be concentrated in an interval). Similarly,
    //I expect all HPET comparators to be connected to the IOAPIC. As such, this monotonic bimap should have O(1) search
    //in normal cases, but will still correctly handle broader cases.
    class MonotonicBimap {
    private:
        Vector<uint8_t> values;
    public:
        void insert(uint8_t value) {
            values.push(value);
        }

        void finalize() {
            values.sort();
            values.shrinkToFit();
        }

        [[nodiscard]] uint8_t valueForIndex(uint8_t index) const {
            return values[index];
        }

        [[nodiscard]] Optional<uint8_t> indexForValue(uint8_t value) const {
            if (values.empty()) return {};
            const auto valueMin = values[0];
            const auto valueMax = values[values.size() - 1];
            if (value < valueMin || value > valueMax) return {};

            // Handle single value case
            if (valueMin == valueMax) {
                return value == valueMin ? Optional<uint8_t>(0) : Optional<uint8_t>{};
            }

            size_t expectedIndex = ((value - valueMin) * (values.size() - 1)) / (valueMax - valueMin);

            // Bounds safety
            expectedIndex = min(expectedIndex, values.size() - 1);

            while (expectedIndex > 0 && values[expectedIndex] > value) {
                expectedIndex--;
            }
            while (expectedIndex < values.size() - 1 && values[expectedIndex] < value) {
                expectedIndex++;
            }

            return values[expectedIndex] == value
                ? Optional<uint8_t>(static_cast<uint8_t>(expectedIndex))
                : Optional<uint8_t>{};
        }

        size_t size() const {
            return values.size();
        }
    };

    WITH_GLOBAL_CONSTRUCTOR(MonotonicBimap, comparatorBimap);
    WITH_GLOBAL_CONSTRUCTOR(MonotonicBimap, ioapicBimap);

    using namespace hal::interrupts;
    CRClass(HPETComparatorSourceDomain, public platform::InterruptDomain, public platform::InterruptEmitter) {
    public:
        size_t getEmitterCount() override {
            return comparatorBimap.size();
        }
    };

    CRClass(HPETRoutingDomain, public platform::InterruptDomain, public platform::ContextIndependentRoutableDomain) {
        HPETRegisters& registers;
    public:
        HPETRoutingDomain(HPETRegisters& regs) : registers(regs) {

        }

        size_t getEmitterCount() override {
            return ioapicBimap.size();
        }

        size_t getReceiverCount() override {
            return comparatorBimap.size();
        }

        bool isRoutingAllowed(size_t fromReceiver, size_t toEmitter) const override {
            auto comparatorIndex = comparatorBimap.valueForIndex(static_cast<uint8_t>(fromReceiver));
            auto& comparatorRegs = registers.comparatorRegs(comparatorIndex);
            uint32_t allowedMappings = comparatorRegs.interruptRouteCapabilities;
            auto pin = ioapicBimap.valueForIndex(static_cast<uint8_t>(toEmitter));
            return (allowedMappings & (1ul << pin)) != 0;
        }

        bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override {
            auto comparatorIndex = comparatorBimap.valueForIndex(static_cast<uint8_t>(fromReceiver));
            auto pin = ioapicBimap.valueForIndex(static_cast<uint8_t>(toEmitter));
            if (!isRoutingAllowed(fromReceiver, toEmitter)) {
                return false;
            }
            registers.comparatorRegs(comparatorIndex).setIOAPICRouting(pin);
            return true;
        }
    };

    class HPETConnector : public platform::DomainConnector {
    public:
        HPETConnector(SharedPtr<platform::InterruptDomain> src, SharedPtr<platform::InterruptDomain> tgt) : platform::DomainConnector(move(src), move(tgt)) {}
        HPETConnector(const HPETConnector&) = default;

        Optional<platform::DomainInputIndex> fromOutput(platform::DomainOutputIndex output) const override {
            return ioapicBimap.valueForIndex(static_cast<uint8_t>(output));
        }

        Optional<platform::DomainOutputIndex> fromInput(platform::DomainInputIndex input) const override {
            auto out = ioapicBimap.indexForValue(static_cast<uint8_t>(input));
            return out ? Optional<platform::DomainOutputIndex>(*out) : Optional<platform::DomainOutputIndex>{};
        }
    };

    using namespace hal::timing;

    constexpr auto hpetBaseFlags = ES_KNOWN_STABLE | ES_FIXED_FREQUENCY | ES_ONESHOT | ES_TRACKS_INTERMEDIATE_TIME;

    esflags_t computeHPETComparatorFlags(HPETRegisters& regs, size_t index) {
        return hpetBaseFlags | (regs.comparatorRegs(index).supportsPeriodicMode() ? ES_PERIODIC : 0);
    }

    class HPETComparatorEventSource : public EventSource {
    private:
        HPETRegisters& regs;
        size_t index;
        bool interruptsEnabled = false;
        bool levelTriggered = false;

        enum class Mode {
            PERIODIC,
            ONESHOT
        };

        Mode mode;

        void ensureInterruptsEnabled(bool enabled = true) {
            if (enabled == interruptsEnabled) return;
            interruptsEnabled = enabled;
            regs.comparatorRegs(index).enableInterrupt(enabled);
        }

        void ensureMode(Mode newMode) {
            if (mode == newMode) return;
            mode = newMode;
            regs.comparatorRegs(index).setPeriodicMode(mode == Mode::PERIODIC);
        }

        void handleInterrupt(interrupts::InterruptFrame& iframe) {
            (void)iframe;
            //klog << "Here 1";
            if (levelTriggered) {
                if (!regs.didTimerRaiseInterrupt(index)) {
                    return;
                }
                regs.acknowledgeTimerInterrupt(index);
            }
            if (callback) {
                (*callback)();
            }
            //klog << "\n";
        }

        using CB = BOUND_METHOD_T(HPETComparatorEventSource, handleInterrupt);
        CB cbhandler;
    public:
        HPETComparatorEventSource(HPETRegisters& r, size_t i, managed::InterruptSourceHandle handle) : EventSource("HPET Comparator", computeHPETComparatorFlags(r, i)), regs(r), index(i){
            mode = r.comparatorRegs(index).isPeriodicMode() ? Mode::PERIODIC : Mode::ONESHOT;
            useLevelTriggered(false);
            _quality = 200;
            _calibrationData = Core::FrequencyData::fromPeriodFs(r.clockPeriod);
            cbhandler = bind_method(this, &HPETComparatorEventSource::handleInterrupt);
            managed::registerHandler(handle, cbhandler);
        }

        void useLevelTriggered(bool lt) {
            this->levelTriggered = lt;
            regs.comparatorRegs(index).generateLevelTriggeredInterrupt(lt);
        }

        void armOneshot(uint64_t deltaTicks) override {
            // Halt counter to avoid race
            //bool wasEnabled = regs.enabled();
            //regs.enable(false);
            /*auto initialValue = regs.getMainTimerValue();
            static uint64_t totalOverhead = 0;
            static uint64_t totalTicks = 0;
            totalTicks++;*/

            //klog << "Arming for " << _calibrationData.ticksToNanos(deltaTicks) << "ns\n";

            ensureInterruptsEnabled(false);
            regs.comparatorRegs(index).comparatorValue = regs.getMainTimerValue() + deltaTicks;

            ensureMode(Mode::ONESHOT);
            ensureInterruptsEnabled();

            //auto tickDuration = regs.getMainTimerValue() - initialValue;
            //totalOverhead += tickDuration;
            //klog << "Operation duration " << _calibrationData.ticksToNanos(tickDuration) << "\n";
            //klog << "Average so far " << _calibrationData.ticksToNanos(totalOverhead / totalTicks) / 1000 << "us\n";
            //regs.enable(wasEnabled);
        }

        [[nodiscard]] uint64_t maxOneshotDelay() const override {
            constexpr uint64_t maxTicks = static_cast<uint64_t>(-1);
            constexpr uint64_t maxTicks32 = (1ul << 32) - 1;
            return regs.comparatorRegs(index).using32BitCounter() ? maxTicks32 : maxTicks;
        }

        void armPeriodic(uint64_t periodTicks) override {
            // Halt counter to avoid race
            bool wasEnabled = regs.enabled();
            regs.enable(false);

            ensureMode(Mode::PERIODIC);
            ensureInterruptsEnabled();

            // Set writable accumulator bit
            regs.comparatorRegs(index).setWritableAccumulator();
            regs.comparatorRegs(index).comparatorValue = periodTicks;

            // Re-enable counter
            regs.enable(wasEnabled);
        }

        [[nodiscard]] uint64_t maxPeriod() const override {
            constexpr uint64_t maxTicks = static_cast<uint64_t>(-1);
            constexpr uint64_t maxTicks32 = (1ul << 32) - 1;
            return regs.comparatorRegs(index).using32BitCounter() ? maxTicks32 : maxTicks;
        }

        void disarm() override {
            ensureInterruptsEnabled(false);
        }

        [[nodiscard]] uint64_t ticksElapsed() override {
            return 0;
        }
    };

    constexpr uint64_t getHPETCounterMask(HPETRegisters& regs) {
        return regs.longCountersSupported() ? (-1ul) : ((1ul << 32) - 1);
    }

    class HPETClockSource : public ClockSource {
        HPETRegisters* regs;
    public:
        HPETClockSource() : ClockSource("Uninitialized HPET Clock Source", 0, CS_FIXED_FREQUENCY | CS_KNOWN_STABLE), regs(nullptr) {}

        HPETClockSource(HPETRegisters& r) : ClockSource("HPET Clock", getHPETCounterMask(r), CS_FIXED_FREQUENCY | CS_KNOWN_STABLE), regs(&r) {
            _quality = 200;
            _calibrationData = Core::FrequencyData::fromPeriodFs(regs -> clockPeriod);
        }

        [[nodiscard]] uint64_t read() const override {
            return regs -> getMainTimerValue();
        }
    };

    //NOTE: according to https://barrelfish.org/publications/intern-rana-hpet.pdf, the HPET is generally assumed to
    //be connected to the first IOAPIC. We shall make that assumption

    SharedPtr<HPETComparatorSourceDomain> comparatorSourceDomain;

    void setupHPETInterruptRouting(HPETRegisters& regs) {
        auto firstIOAPIC = interrupts::getFirstIOAPIC();
        const auto ioapicLineCount = firstIOAPIC -> getReceiverCount();
        const uint32_t mask = static_cast<uint32_t>((1ul << ioapicLineCount) - 1);
        uint32_t possibleIOAPICLines = 0;
        for (uint8_t i = 0; i < regs.comparatorCount(); i++) {
            auto& comparator = regs.comparatorRegs(i);
            if (comparator.interruptRouteCapabilities & mask) {
                comparatorBimap.insert(i);
                possibleIOAPICLines |= comparator.interruptRouteCapabilities;
            }
            else if (!comparator.supportsFSBRouting()) {
                klog << "Comparator at index " << i << " does not support FSB routing and is not connected to the IOAPIC. This is strange.\n";
            }
        }
        possibleIOAPICLines &= mask;
        for (uint8_t i = 0; possibleIOAPICLines != 0; i++, possibleIOAPICLines >>= 1) {
            if (possibleIOAPICLines & 1) {
                ioapicBimap.insert(i);
            }
        }
        comparatorBimap.finalize();
        ioapicBimap.finalize();

        auto routingDomain = make_shared<HPETRoutingDomain>(regs);
        comparatorSourceDomain = make_shared<HPETComparatorSourceDomain>();
        auto ioapicConnector = make_shared<HPETConnector>(routingDomain, firstIOAPIC);
        auto comparatorConnector = make_shared<platform::AffineConnector>(comparatorSourceDomain, routingDomain, 0, 0, comparatorBimap.size());
        topology::registerDomain(routingDomain);
        topology::registerDomain(comparatorSourceDomain);
        topology::registerConnector(ioapicConnector);
        topology::registerConnector(comparatorConnector);
    }

    void registerHPETEventSources(HPETRegisters& regs) {
        auto firstIOAPIC = interrupts::getFirstIOAPIC();
        const auto ioapicLineCount = firstIOAPIC -> getReceiverCount();
        const uint32_t mask = static_cast<uint32_t>((1ul << ioapicLineCount) - 1);
        size_t linearIndex = 0;
        for (size_t i = 0; i < regs.comparatorCount(); i++) {
            auto& comparator = regs.comparatorRegs(i);
            if (comparator.interruptRouteCapabilities & mask) {
                auto sourceHandle = managed::InterruptSourceHandle(comparatorSourceDomain, linearIndex);
                linearIndex++;
                auto* eventSource = new HPETComparatorEventSource(regs, i, sourceHandle);
                timing::registerEventSource(*eventSource);
            }
        }
    }

    HPETClockSource cs;

    bool initHPET(){
        auto hpetTablePtr = acpi::optional<acpi::HPET>();
        if (hpetTablePtr == nullptr) {
            return false;
        }
        auto& hpetTable = *hpetTablePtr;
        klog << "Found HPET with address info " << (void*)hpetTable.hpetBaseAddress.address << "\n";
        klog << "HPET address space ID is " << hpetTable.hpetBaseAddress.addressSpaceID << "\n";
        auto& base = mapHPET(hpetTable);
        assert(base.clockPeriod <= maximumClockPeriod, "HPET clock period is too large");
        auto hpetFreq = Core::FrequencyData::fromPeriodFs(base.clockPeriod);
        klog << "HPET clock frequency is " << hpetFreq << "\n";
        klog << "base.comparatorCount() = " << base.comparatorCount() << "\n";
        klog << "base.longCountersSupported() = " << base.longCountersSupported() << "\n";

        setupHPETInterruptRouting(base);
        registerHPETEventSources(base);
        new(&cs)HPETClockSource(base);
        timing::registerClockSource(cs);

        base.enable();

        return true;
    }
}
#pragma GCC diagnostic pop
