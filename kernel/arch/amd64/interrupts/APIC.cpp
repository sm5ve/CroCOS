//
// Created by Spencer Martin on 8/27/25.
//
#include <timing/timing.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/amd64.h>
#include <arch/amd64/smp.h>
#include <core/ds/Trees.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/interrupts/LegacyPIC.h>

namespace kernel::amd64::interrupts{
    constexpr uint32_t IOAPIC_REG_ID = 0x00;
    constexpr uint32_t IOAPIC_REG_VERSION = 0x01;
    constexpr uint32_t IOAPIC_REG_ARBITRATION_PRIORITY = 0x02;
    constexpr uint32_t IOAPIC_REG_REDIRECT_TABLE_BASE = 0x10;

    constexpr size_t IOAPIC_VECTOR_MAPPING_BASE = 0x10;

    constexpr uint32_t IA32_APIC_BASE_MSR = 0x1B;
    constexpr uint32_t IA32_APIC_BASE_MSR_ENABLE = 1u << 11;

    SharedPtr<IOAPIC> firstIOAPIC;

    IOAPIC::IOAPIC(const uint8_t i, void* m, const uint32_t g) : id(i), mmio_window(static_cast<volatile uint32_t*>(m)), gsi_base(g) {
        const uint32_t version = regRead(IOAPIC_REG_VERSION);
        lineCount = (version >> 16) & 0xffu;
        activationTypes = make_unique_array<Optional<hal::interrupts::InterruptLineActivationType>>(lineCount);
    }

    IOAPIC::~IOAPIC() {}

    uint32_t IOAPIC::regRead(const uint8_t index) const{
        mmio_window[0] = static_cast<uint32_t>(index) & 0xffu;
        return mmio_window[4];
    }

    void IOAPIC::regWrite(const uint8_t index, const uint32_t value){
        mmio_window[0] = static_cast<uint32_t>(index) & 0xffu;
        mmio_window[4] = value;
    }

    uint8_t getRegStartForLineIndex(const size_t lineIndex){
        return static_cast<uint8_t>(lineIndex * 2 + 0x10);
    }

    void IOAPIC::setActivationTypeByGSI(const uint32_t gsi, const hal::interrupts::InterruptLineActivationType type) {
        setActivationType(gsi + gsi_base, type);
    }

    void IOAPIC::setActivationType(const size_t receiver, const hal::interrupts::InterruptLineActivationType type) {
        assert(receiver < lineCount, "gsi out of range");
        auto regVal = regRead(getRegStartForLineIndex(receiver));
        constexpr uint32_t polarity_mask = 1u << 13;
        constexpr uint32_t trigger_mask = 1u << 15;
        regVal &= ~(polarity_mask | trigger_mask);
        if (isLevelTriggered(type)) regVal |= trigger_mask;
        if (isLowTriggered(type)) regVal |= polarity_mask;
        regWrite(getRegStartForLineIndex(receiver), regVal);
        activationTypes[receiver] = type;
    }

    Optional<hal::interrupts::InterruptLineActivationType> IOAPIC::getActivationType(size_t receiver) const {
        if (receiver >= lineCount) return {};
        return activationTypes[receiver];
    }
    
    void IOAPIC::setUninitializedActivationTypes(hal::interrupts::InterruptLineActivationType type) {
        for (size_t i = 0; i < lineCount; i++) {
            if (!activationTypes[i].occupied()) {
                setActivationType(i, type);
            }
        }
    }

    void IOAPIC::setNonmaskable(uint32_t gsi, bool nonmaskable) {
        assert(gsi - gsi_base < lineCount, "gsi out of range");
        auto regVal = regRead(getRegStartForLineIndex(gsi - gsi_base));
        constexpr uint32_t delivery_mask = 5u << 8;
        regVal &= ~delivery_mask;
        if (nonmaskable) {
            regVal |= 4u << 8;
        }
        regWrite(getRegStartForLineIndex(gsi - gsi_base), regVal);
    }

    size_t IOAPIC::getReceiverCount(){
        return lineCount;
    }

    size_t IOAPIC::getEmitterCount(){
        //From the OSDev wiki: allowed values for interrupt vectors are 0x10 to 0xFE
        return (INTERRUPT_VECTOR_COUNT - 2) - IOAPIC_VECTOR_MAPPING_BASE + 1ul;
    }

    bool IOAPIC::routeInterrupt(const size_t lineIndex, size_t destinationLine){
        //All emitters are indexed starting at 0, so destinationLine = 0 should correspond to interrupt number 16
        destinationLine += IOAPIC_VECTOR_MAPPING_BASE;
        if (destinationLine < 0x10 || destinationLine > 0xFE) return false;
        if (lineIndex >= lineCount) return false;
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        regVal &= ~0xffu;
        regVal |= (static_cast<uint32_t>(destinationLine) & 0xff);
        regWrite(getRegStartForLineIndex(lineIndex), regVal);
        return true;
    }

    constexpr uint32_t IOAPIC_MASK_BIT = 1u << 16;

    bool IOAPIC::isReceiverMasked(const size_t lineIndex) const{
        assert(lineIndex < lineCount, "lineIndex out of range");
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        return (regVal & IOAPIC_MASK_BIT) != 0;
    }

    void IOAPIC::setReceiverMask(const size_t lineIndex, const bool shouldMask){
        assert(lineIndex < lineCount, "lineIndex out of range");
        auto regVal = regRead(getRegStartForLineIndex(lineIndex));
        regVal &= ~IOAPIC_MASK_BIT;
        if (shouldMask) regVal |= IOAPIC_MASK_BIT;
        regWrite(getRegStartForLineIndex(lineIndex), regVal);
    }

    uint32_t IOAPIC::getGSIBase() const {
        return gsi_base;
    }


    struct IOAPIC_GSI_Comparator {
        bool operator()(const SharedPtr<IOAPIC>& a, const SharedPtr<IOAPIC>& b){
            return a -> getGSIBase() < b -> getGSIBase();
        }
    };

    IRQDomain::IRQDomain(size_t mapping[16]){
        memcpy(surjectiveMapping, mapping, sizeof(surjectiveMapping));
        maxMapping = 0;
        for (auto i : surjectiveMapping) {
            maxMapping = max(i, maxMapping);
        }
    }

    size_t IRQDomain::getEmitterCount() {
        return maxMapping + 1;
    }

    size_t IRQDomain::getReceiverCount() {
        return 16;
    }

    size_t IRQDomain::getEmitterFor(size_t receiver) const{
        assert(receiver < 16, "receiver out of range");
        return surjectiveMapping[receiver];
    }

    IRQToIOAPICConnector::IRQToIOAPICConnector(SharedPtr<IRQDomain> irqDomain, SharedPtr<InterruptDomain> ioapic, Bimap<size_t, size_t> &&map) :
    DomainConnector(static_pointer_cast<InterruptDomain>(move(irqDomain)), move(ioapic)), irqToAPICLineMap(move(map)){}

    Optional<DomainInputIndex> IRQToIOAPICConnector::fromOutput(DomainOutputIndex index) const {
        if (irqToAPICLineMap.contains(index)) {
            return irqToAPICLineMap.at(index);
        }
        return {};
    }

    Optional<DomainOutputIndex> IRQToIOAPICConnector::fromInput(DomainInputIndex index) const {
        if (irqToAPICLineMap.containsRight(index)) {
            return irqToAPICLineMap.atRight(index);
        }
        return {};
    }

    using IOAPIC_Tree = RedBlackTree<SharedPtr<IOAPIC>, IOAPIC_GSI_Comparator>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_Tree, ioapicsByGSI);
    using IOAPIC_ID_Map = HashMap<size_t, SharedPtr<IOAPIC>>;
    WITH_GLOBAL_CONSTRUCTOR(IOAPIC_ID_Map, ioapicsByID);

    void createIOAPICStructures(acpi::MADT& madt) {
        for (const auto ioapicEntry : madt.entries<acpi::MADT_IOAPIC_Entry>()) {
            uintptr_t base = ioapicEntry.ioapicAddress;
            auto* mmio_window = PageTableManager::temporaryHackMapMMIOPage(mm::phys_addr(base));
            auto gsiBase = ioapicEntry.gsiBase;
            auto ioapic = make_shared<IOAPIC>(ioapicEntry.ioapicID, mmio_window, gsiBase);
            if (!firstIOAPIC || firstIOAPIC -> getGSIBase() > gsiBase) {
                firstIOAPIC = ioapic;
            }
            ioapicsByID[ioapicEntry.ioapicID] = ioapic;
            ioapicsByGSI.insert(ioapic);
            hal::interrupts::topology::registerDomain(ioapic);

            auto apicConnector = make_shared<AffineConnector>(ioapic,
            getLAPICDomain(), IOAPIC_VECTOR_MAPPING_BASE, 0, ioapic -> getEmitterCount());
            hal::interrupts::topology::registerConnector(apicConnector);
        }
    }

    SharedPtr<IOAPIC> getIOAPICForGSI(uint32_t gsi) {
        SharedPtr<IOAPIC> apicForGSI;
        bool found = ioapicsByGSI.mappedFloor(gsi, apicForGSI, [](SharedPtr<IOAPIC> apic) {
            return apic -> getGSIBase();
        });
        if (!found) {return SharedPtr<IOAPIC>();}
        if (gsi - apicForGSI -> getGSIBase() >= apicForGSI -> getReceiverCount()) {
            return SharedPtr<IOAPIC>();
        }
        return apicForGSI;
    }

    hal::interrupts::InterruptLineActivationType getActivationTypeFromMADTFlags(uint16_t flags) {
        bool activeHigh = (flags & 2) == 0;
        bool edgeTriggered = (flags & 8) == 0;
        return hal::interrupts::activationTypeForLevelAndTriggerMode(activeHigh, edgeTriggered);
    }

    SharedPtr<IOAPIC> addIRQDomainConectorMapping(Optional<size_t>* irqToEmitterMap, size_t& emitterMax, HashMap<SharedPtr<IOAPIC>, SharedPtr<Bimap<size_t, size_t>>>& connectorMapsByIOAPIC, uint8_t irqSource, uint32_t gsi) {
        assert(irqSource < 16, "irqSource out of range");

        auto ioapic = getIOAPICForGSI(gsi);
        assert(ioapic, "No IOAPIC for GSI");
        if (!connectorMapsByIOAPIC.contains(ioapic)) {
            connectorMapsByIOAPIC.insert(ioapic, make_shared<Bimap<size_t, size_t>>());
        }

        const auto bimap = connectorMapsByIOAPIC[ioapic];
        //If the GSI we're trying to map to is already associated with an emitter, just update the emitter map
        //but no changes need to be made to the connector
        if (bimap -> containsRight(gsi)) {
            irqToEmitterMap[irqSource] = bimap -> atRight(gsi);
            return ioapic;
        }
        //Otherwise, we need to create a new emitter
        assert(!irqToEmitterMap[irqSource].occupied(), "why are we calling this on an already mapped irq source?");
        size_t emitterIndex = emitterMax++;
        irqToEmitterMap[irqSource] = emitterIndex;

        bimap -> insert(emitterIndex, gsi - ioapic -> getGSIBase());
        return ioapic;
    }

    SharedPtr<IRQDomain> irqDomain;

    void createIRQDomainConnectorsAndConfigureIOAPICActivationType(acpi::MADT& madt) {
        Optional<size_t> irqToEmitterMap[16];
        size_t emitterMax = 0;
        //maps IRQDomain emitter index to IOAPIC line (so gsi - gsi_base)
        HashMap<SharedPtr<IOAPIC>, SharedPtr<Bimap<size_t, size_t>>> connectorMapsByIOAPIC;
        uint16_t mappedIRQs = 0;
        //For every source override entry, we configure the IOAPIC activation type accordingly
        //and add the mapping to a bimap
        for (const auto& sourceOverride : madt.entries<acpi::MADT_IOAPIC_Source_Override_Entry>()) {
            if (sourceOverride.busSource != 0) {
                klog << "Warning: MADT interrupt source override entry lists non-ISA bus source.\n";
            }
            if (mappedIRQs & (1u << sourceOverride.irqSource)) {
                klog << "Warning: MADT interrupt source override entry lists duplicate interrupt source. Skipping.\n";
                continue;
            }
            auto ioapic = addIRQDomainConectorMapping(irqToEmitterMap, emitterMax, connectorMapsByIOAPIC, sourceOverride.irqSource, sourceOverride.gsi);
            ioapic -> setActivationTypeByGSI(sourceOverride.gsi, getActivationTypeFromMADTFlags(sourceOverride.flags));
            mappedIRQs |= 1u << sourceOverride.irqSource;
        }
        for (uint32_t i = 0; i < 16; i++) {
            if (mappedIRQs & (1u << i)) {
                continue;
            }
            addIRQDomainConectorMapping(irqToEmitterMap, emitterMax, connectorMapsByIOAPIC, static_cast<uint8_t>(i), i);
        }
        size_t finalizedEmitterMap[16];
        for (auto i = 0; i < 16; i++) {
            assert(irqToEmitterMap[i].occupied(), "irqToEmitterMap[i] not occupied");
            finalizedEmitterMap[i] = *irqToEmitterMap[i];
        }

        irqDomain = make_shared<IRQDomain>(finalizedEmitterMap);
        hal::interrupts::topology::registerDomain(irqDomain);
        for (const auto& connectorInfo : connectorMapsByIOAPIC) {
            auto ioapic = connectorInfo.first();
            auto bimap = connectorInfo.second();
            auto connector = make_shared<IRQToIOAPICConnector>(irqDomain, ioapic, move(*bimap));
            hal::interrupts::topology::registerConnector(connector);
        }
    }

    uint64_t getLAPICBaseMask() {
        uint32_t eax, ebx, ecx, edx;
        cpuid(eax, ebx, ecx, edx, 0x80000000);
        uint64_t bits = 36;
        if (eax >= 0x80000008) {
            cpuid(eax, ebx, ecx, edx, 0x80000008);
            bits = eax & 0xff;
        }
        uint64_t mask = (1ull << bits) - 1;
        return mask & ~(0xffful);
    }

    uint64_t getLAPICBase() {
        return rdmsr(IA32_APIC_BASE_MSR) & getLAPICBaseMask();
    }

    constexpr uint32_t LAPIC_SPURIOUS_INTERRUPT_VECTOR_REGISTER = 0xF0;
    constexpr uint8_t LAPIC_SPURIOUS_INTERRUPT_VECTOR = 0xFF;
    constexpr uint32_t LAPIC_EOI_REGISTER = 0xB0;

    LAPIC::LAPIC(mm::phys_addr paddr) {
        auto mmio = PageTableManager::temporaryHackMapMMIOPage(paddr);
        mmio_window = static_cast<Register<uint32_t>*>(mmio);
        reg(LAPIC_SPURIOUS_INTERRUPT_VECTOR_REGISTER) = (0x100u | LAPIC_SPURIOUS_INTERRUPT_VECTOR);
    }

    Register<uint32_t>& LAPIC::reg(size_t offset) {
        return mmio_window[offset/sizeof(uint32_t)];
    }

    size_t LAPIC::getEmitterCount() {
        return hal::CPU_INTERRUPT_COUNT;
    }

    size_t LAPIC::getReceiverCount() {
        return hal::CPU_INTERRUPT_COUNT;
    }

    size_t LAPIC::getEmitterFor(size_t receiver) const {
        return receiver;
    }

    constexpr size_t destinationOffset = 24;
    constexpr size_t ipiRegHigh = 0x310;
    constexpr size_t ipiRegLow = 0x300;

    constexpr uint32_t sendPendingBit = 1 << 12;

    bool LAPIC::issueIPI(IPIRequest request) {
        uint32_t lowDWord = reg(ipiRegLow);
        if (lowDWord & sendPendingBit) {
            return false;
        }

        uint32_t highDWord = reg(ipiRegHigh);

        highDWord &= ~(0xfffu << destinationOffset);
        auto destinationLAPIC = static_cast<uint32_t>(smp::getProcessorInfoForProcessorID(request.target).lapicID);
        //klog << "destinationLAPIC: " << destinationLAPIC << "\n";
        highDWord |= (destinationLAPIC << destinationOffset);

        lowDWord &= ~(0xffffu | (3 << 18));
        lowDWord |= request.vector;

        switch (request.type) {
            case STANDARD:
                lowDWord |= (0 << 8); break;
            case INIT:
                lowDWord |= (5 << 8); break;
            case SIPI:
                lowDWord |= (6 << 8); break;
        }

        lowDWord |= (request.level ? (1 << 14) : 0);

        reg(ipiRegHigh) = highDWord;
        reg(ipiRegLow) = lowDWord;
        return true;
    }

    void LAPIC::issueIPISync(IPIRequest request) {
        while (!issueIPI(request)) {
            asm volatile("pause");
        }
        waitForIPIToSend();
    }


    bool LAPIC::ipiStillSending() {
        return reg(ipiRegLow) & sendPendingBit;
    }

    void LAPIC::waitForIPIToSend() {
        auto& r = reg(ipiRegLow);
        while (r & sendPendingBit) {
            asm volatile("pause");
        }
    }



    void LAPIC::issueEOI(InterruptFrame &iframe) {
        (void)iframe;
        reg(LAPIC_EOI_REGISTER) = 0;
    }

    LAPIC::~LAPIC() {

    }

    SharedPtr<IRQDomain> getIRQDomain() {
        return irqDomain;
    }

    SharedPtr<LAPIC> lapicDomain;

    SharedPtr<LAPIC> getLAPICDomain() {
        return  lapicDomain;
    }

    //Used by HPET
    SharedPtr<IOAPIC> getFirstIOAPIC() {
        return firstIOAPIC;
    }

    CRClass(SpuriousInterruptDomain, public InterruptDomain, public InterruptEmitter) {
    public:
        size_t getEmitterCount() override {
            return 1;
        }
    };

    constexpr size_t MAX_LVT_SIZE = 7;
    constexpr uint32_t LVT_OFFSETS[MAX_LVT_SIZE] = {0x2f0, 0x320, 0x330, 0x340, 0x350, 0x360, 0x370};
    constexpr uint32_t LAPIC_TIMER_LVT_ENTRY = 0x320;
    constexpr uint32_t LAPIC_TIMER_MODE_OFFSET = 17;

    constexpr uint32_t LAPIC_EOI_REG = 0xB0;

    enum class LAPICTimerMode : uint32_t {
        OneShot = 0,
        Periodic = 1,
        TSCDeadline = 2
    };

    class LAPICLocalDeviceRoutingDomain;
    class LAPICLocalDeviceEmitters;

    SharedPtr<SpuriousInterruptDomain> spuriousInterruptDomain;
    SharedPtr<LAPICLocalDeviceEmitters> localDeviceEmitters;
    SharedPtr<LAPICLocalDeviceRoutingDomain> localDeviceRouter;

    using namespace hal::interrupts;

    CRClass(LAPICLocalDeviceRoutingDomain, public FreeRoutableDomain, public InterruptDomain, public EOIDomain, public ConfigurableActivationTypeDomain) {
        LAPIC& lapic;
        public:
        LAPICLocalDeviceRoutingDomain(LAPIC& l) : lapic(l) {
            for (size_t i = 0; i < MAX_LVT_SIZE; i++) {
                maskInterrupt(i, true);
            }
        }

        size_t getEmitterCount() override {
            return hal::CPU_INTERRUPT_COUNT;
        }

        size_t getReceiverCount() override {
            //Not all processors will support all 7 LVT entries... but anything modern should.
            //We'll expose them all for now and refine this if needed.
            return MAX_LVT_SIZE;
        }

        bool routeInterrupt(size_t fromReceiver, size_t toEmitter) override {
            assert(fromReceiver < MAX_LVT_SIZE, "fromReceiver out of range");
            auto& reg = lapic.reg(LVT_OFFSETS[fromReceiver]);
            reg &= ~(0xffu);
            reg |= toEmitter & 0xffu;
            return true;
        }

        void issueEOI(InterruptFrame &iframe) override {
            (void)iframe;
            lapic.reg(LAPIC_EOI_REG) = 0;
        }

        void maskInterrupt(size_t index, bool shouldMask = true) {
            assert(index < MAX_LVT_SIZE, "index out of range");
            auto& reg = lapic.reg(LVT_OFFSETS[index]);
            if (shouldMask) {
                reg |= 1u << 16;
            }
            else {
                reg &= ~(1u << 16);
            }
        }

        void setActivationType(size_t index, InterruptLineActivationType activationType) override {
            (void)index; (void)activationType;
            //TODO implement
        }

        Optional<InterruptLineActivationType> getActivationType(size_t receiver) const override {
            (void)receiver;
            //TODO implement
            return InterruptLineActivationType::EDGE_HIGH;
        }

        //TODO expose methods to set delivery mode, polarity, trigger mode, remote IRR
    };

    CRClass(LAPICLocalDeviceEmitters, public InterruptDomain, public InterruptEmitter) {
        public:
        size_t getEmitterCount() override {
            return MAX_LVT_SIZE;
        }

        static managed::InterruptSourceHandle timerHandle() {
            return {static_pointer_cast<InterruptDomain>(localDeviceEmitters), 1};
        }
    };

    using namespace kernel::hal::timing;

    constexpr uint32_t LAPIC_TIMER_INITIAL_COUNT_REGISTER = 0x380;
    constexpr uint32_t LAPIC_TIMER_CURRENT_COUNT_REGISTER = 0x390;
    constexpr uint32_t LAPIC_TIMER_DIVIDE_CONFIG_REGISTER = 0x3E0;
    constexpr auto LAPIC_MAX_TIMER_VALUE = static_cast<uint32_t>(-1);


    class LAPICTimer : public EventSource{
        static constexpr auto deviceFlags = ES_KNOWN_STABLE | ES_ONESHOT | ES_PERIODIC | ES_PERCPU | ES_TRACKS_INTERMEDIATE_TIME;
        LAPIC& lapic;
        uint64_t lastArmCounter;

        void executeCallback(InterruptFrame& iframe) {
            (void)iframe;
            if (callback) {
                (*callback)();
            }
        }

        BOUND_METHOD_T(LAPICTimer, executeCallback) callbackExecuter = bind_method(this, &LAPICTimer::executeCallback);
        LAPICTimerMode currentMode;
        bool disarmed = true;

        void ensureMode(LAPICTimerMode mode) {
            if (currentMode != mode) {
                auto& reg = lapic.reg(LAPIC_TIMER_DIVIDE_CONFIG_REGISTER);
                reg &= ~(0b11u << LAPIC_TIMER_MODE_OFFSET);
                reg |= (static_cast<uint32_t>(mode) << LAPIC_TIMER_MODE_OFFSET);
                currentMode = mode;
            }
        }

        void ensureDisarmed(const bool status = true) {
            if (status != disarmed) {
                disarmed = status;
                localDeviceRouter -> maskInterrupt(1, status);
            }
        }
    public:
        LAPICTimer(LAPIC& l) : EventSource("LAPIC", deviceFlags), lapic(l) {
            managed::registerHandler(LAPICLocalDeviceEmitters::timerHandle(), callbackExecuter);

            auto& modeReg = lapic.reg(LAPIC_TIMER_LVT_ENTRY);
            modeReg &= ~(0b11u << LAPIC_TIMER_MODE_OFFSET);
            modeReg |= (static_cast<uint32_t>(LAPICTimerMode::OneShot) << LAPIC_TIMER_MODE_OFFSET);
            currentMode = LAPICTimerMode::OneShot;

            auto& dividerReg = lapic.reg(LAPIC_TIMER_DIVIDE_CONFIG_REGISTER);
            dividerReg &= ~(0xbu);
            dividerReg |= 0xbu;

            _quality = 300;
        };

        void armOneshot(uint64_t deltaTicks) override {
            ensureMode(LAPICTimerMode::OneShot);
            ensureDisarmed(false);
            //TODO add support for TSC deadline
            const auto initialCount = static_cast<uint32_t>(deltaTicks);
            lastArmCounter = initialCount;
            lapic.reg(LAPIC_TIMER_INITIAL_COUNT_REGISTER) = initialCount;
        }

        [[nodiscard]] uint64_t maxOneshotDelay() const override {
            return LAPIC_MAX_TIMER_VALUE;
        }

        void armPeriodic(uint64_t periodTicks) override {
            ensureMode(LAPICTimerMode::Periodic);
            ensureDisarmed(false);
            const auto initialCount = static_cast<uint32_t>(periodTicks);
            lastArmCounter = initialCount;
            lapic.reg(LAPIC_TIMER_INITIAL_COUNT_REGISTER) = initialCount;
        }

        [[nodiscard]] uint64_t maxPeriod() const override{
            return LAPIC_MAX_TIMER_VALUE;
        }

        void disarm() override {
            ensureDisarmed();
        }

        [[nodiscard]] uint64_t ticksElapsed() override {
            return lastArmCounter - lapic.reg(LAPIC_TIMER_CURRENT_COUNT_REGISTER);
        }
    };

    bool enableAPIC() {
        auto lapicBasePhysical = getLAPICBase();
        auto lapicMSRNewVal = lapicBasePhysical | IA32_APIC_BASE_MSR_ENABLE;
        klog << "Enabling APIC, writing MSR value " << reinterpret_cast<void*>(lapicMSRNewVal) << "\n";
        wrmsr(IA32_APIC_BASE_MSR, lapicMSRNewVal);
        return true;
    }

    void setupAPICs(acpi::MADT& madt) {
        enableAPIC();
        auto lapicBasePhysical = getLAPICBase();
        lapicDomain = make_shared<LAPIC>(mm::phys_addr(lapicBasePhysical));
        hal::interrupts::topology::registerDomain(lapicDomain);
        auto lapicConnector = make_shared<AffineConnector>(lapicDomain, getCPUInterruptVectors(), 0, 0, hal::CPU_INTERRUPT_COUNT);
        hal::interrupts::topology::registerConnector(lapicConnector);
        spuriousInterruptDomain = make_shared<SpuriousInterruptDomain>();
        hal::interrupts::topology::registerDomain(spuriousInterruptDomain);
        auto spuriousConnector = make_shared<AffineConnector>(spuriousInterruptDomain, getCPUInterruptVectors(), LAPIC_SPURIOUS_INTERRUPT_VECTOR, 0, 1);
        hal::interrupts::topology::registerExclusiveConnector(spuriousConnector);
        localDeviceEmitters = make_shared<LAPICLocalDeviceEmitters>();
        localDeviceRouter = make_shared<LAPICLocalDeviceRoutingDomain>(*lapicDomain);
        hal::interrupts::topology::registerDomain(localDeviceEmitters);
        hal::interrupts::topology::registerDomain(localDeviceRouter);
        auto localDeviceEmitterConnector = make_shared<AffineConnector>(localDeviceEmitters, localDeviceRouter, 0, 0, MAX_LVT_SIZE);
        hal::interrupts::topology::registerConnector(localDeviceEmitterConnector);
        auto localDeviceRouterConnector = make_shared<AffineConnector>(localDeviceRouter, getCPUInterruptVectors(), 0, 0, hal::CPU_INTERRUPT_COUNT);
        hal::interrupts::topology::registerConnector(localDeviceRouterConnector);
        createIOAPICStructures(madt);
        createIRQDomainConnectorsAndConfigureIOAPICActivationType(madt);
        for (const auto& ioapic : ioapicsByID.values()) {
            ioapic -> setUninitializedActivationTypes(hal::interrupts::activationTypeForLevelAndTriggerMode(true, false));
        }
        klog << "Enabled APIC\n";

        auto timer = new LAPICTimer(*lapicDomain);
        timing::registerEventSource(*timer);
    }

    constexpr size_t LAPIC_ID_REG = 0x20;

    uint32_t LAPIC::getID() {
        return (reg(LAPIC_ID_REG) >> 24) & 0xff;
    }
}