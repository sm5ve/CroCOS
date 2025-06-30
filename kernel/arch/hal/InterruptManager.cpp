//
// Created by Spencer Martin on 4/25/25.
//

#include <arch/hal/interrupts.h>
namespace kernel::hal::interrupts{
    namespace topology{
        struct InterruptSourceInfo{
            Optional<InterruptGroupHandle> group;
            Optional<InterruptReceiver> receiver;
        };

        using SourceInfoMapType = HashMap<InterruptSource, InterruptSourceInfo>;
        using InterruptSourceGroups = HashMap<InterruptGroupHandle, Vector<InterruptSource>>;
        using InterruptReceiverToSourcesMap = HashMap<InterruptReceiver, Vector<InterruptSource>>;

        WITH_GLOBAL_CONSTRUCTOR(Vector<platform::IInterruptController*>, controllers);
        WITH_GLOBAL_CONSTRUCTOR(SourceInfoMapType, sourceInfo);
        WITH_GLOBAL_CONSTRUCTOR(InterruptSourceGroups, sourceGroups);
        WITH_GLOBAL_CONSTRUCTOR(RWSpinlock, topologyLock);
        WITH_GLOBAL_CONSTRUCTOR(InterruptReceiverToSourcesMap, receiverToSourcesMap);

        void registerController(platform::IInterruptController& c){
            WriterLockGuard guard(topologyLock);
            controllers.push(&c);
            //I'm not really sure what logic to put in here yet. But it seems good to keep track of all of our
            //interrupt controllers
        }

        void registerSource(InterruptSource s, Optional<InterruptGroupHandle> h){
            WriterLockGuard guard(topologyLock);
            sourceInfo.insert(s, {.group = h, .receiver = {}});
            if(h.occupied()){
                if(!sourceGroups.contains(*h)){
                    sourceGroups.insert(*h, Vector<InterruptSource>(1));
                }
                sourceGroups.at(*h).push(s);
            }
        }

        void bindSourceToLine(InterruptSource s, InterruptReceiver r){
            WriterLockGuard guard(topologyLock);
            assert(sourceInfo.contains(s), "Tried to bind unregistered source to line");
            assert(!sourceInfo.at(s).receiver.occupied(), "Tried to bind a source to multiple receivers");
            sourceInfo.at(s).receiver = r;
            if(!receiverToSourcesMap.contains(r)){
                receiverToSourcesMap.insert(r, Vector<InterruptSource>());
            }
            receiverToSourcesMap.at(r).push(s);
        }

        using InterruptMapping = Optional<Tuple<platform::VectorIndex, Optional<InterruptCPUAffinity>>>;

        InterruptMapping findVectorForSource(InterruptSource s){
            ReaderLockGuard guard(topologyLock);
            //Prevent infinite loops when resolving a vector mapping.
            //An acyclic route will go through a sequence of pairwise distinct interrupt sources, so the total
            //number of interrupts is a conservative upper bound for the length of an acyclic route.
            //In practice, cycles should never be possible - it would make no sense from a hardware perspective to loop
            //the output of an interrupt controller back into itself.
            InterruptSource traversedSource = s;
            for(size_t i = 0; i < sourceInfo.size(); i++){
                auto r = sourceInfo.at(traversedSource).receiver;
                if(!r.occupied()) return {};
                auto& c = *(r -> owner);
                //If this is a cascaded controller, then the controller maps each of its receivers to another
                //interrupt source, which is then connected to another controller
                if(platform::hasFeature(c.getFeatureSet(), platform::InterruptControllerFeature::CascadedController)){
                    auto& sub = reinterpret_cast<platform::ISubordinateController&>(c);
                    auto mapped = sub.getMapping(*r);
                    //If no mapping was set for the subordinate controller, return empty
                    if(!mapped.occupied()) return {};
                    //Otherwise, update our traversed source to the corresponding output of the interrupt controller and repeat
                    traversedSource = *mapped;
                }
                //Otherwise, if it's a terminal controller, get the vector and return
                else if(platform::hasFeature(c.getFeatureSet(), platform::terminalController)){
                    auto& term = reinterpret_cast<platform::ITerminalController&>(c);
                    return term.getMapping(*r);
                }
                else{
                    assertNotReached("Somehow controller was neither subordinate nor terminal...\n");
                    return {};
                }
            }
            return {};
        }
    }

    namespace managed{
        using HandlerMap = HashMap<InterruptSource, IInterruptHandler*>;

        struct AffinityData {
            AffinityRequestData requestData;
            Optional<InterruptCPUAffinity> requestedAffinity;
        };

        using AffinityRequestMap = HashMap<InterruptSource, AffinityData>;
        WITH_GLOBAL_CONSTRUCTOR(HandlerMap, interuptHandlers);
        WITH_GLOBAL_CONSTRUCTOR(AffinityRequestMap, affinityRequestMap);
        WITH_GLOBAL_CONSTRUCTOR(RWSpinlock, handlerLock);

        bool registerHandler(InterruptSource s, IInterruptHandler& h){
            WriterLockGuard guard(handlerLock);
            if(interuptHandlers.contains(s)) return false;
            interuptHandlers.insert(s, &h);
            return true;
        }

        bool unregisterHandler(InterruptSource s, IInterruptHandler& h){
            WriterLockGuard guard(handlerLock);
            if(!interuptHandlers.contains(s)) return false;
            if(!(interuptHandlers.at(s)->operator==(h))) return false;
            interuptHandlers.remove(s);
            return true;
        }

        bool acknowledge(InterruptSource s) {
            auto v = topology::findVectorForSource(s);
            if (!v.occupied()) return false;
            //Affinity shouldn't matter, we just need the vector
            platform::issueEOI(v -> get<0>());
            return true;
        }

        AffinityRequest requestCPUAffinity(InterruptSource s, InterruptCPUAffinity aff) {
            if (!affinityRequestMap.contains(s)) {
                affinityRequestMap.insert(s, {});
            }
            auto& r = affinityRequestMap.at(s);
            r.requestedAffinity = aff;
            return r.requestData;
        }

        Optional<InterruptCPUAffinity> getAffinity(InterruptSource s) {
            auto m = topology::findVectorForSource(s);
            if (!m.occupied()) {
                return {};
            }
            return m -> get<1>();
        }

        void commitVectorReassignment() {

        }

        /*InterruptMaskState requestInterruptMask(InterruptSource, bool masked);
        InterruptMaskState getInterruptMaskState(InterruptSource);
        void recomputeVectorAssignments();
        //Used on x86 for reserving vectors 0-31 for exceptions, as well as a vector for syscalls
        Vector<Tuple<InterruptSource, platform::VectorIndex>> reserveVectorRange(platform::VectorIndex start, platform::VectorIndex end);
        void handleInterrupt(hal::InterruptFrame& frame);
        */
    }
}

using namespace kernel::hal::interrupts;
Core::PrintStream& operator<<(Core::PrintStream& ps, NontargetedAffinityTypes& affinityType){
    switch (affinityType) {
        case NontargetedAffinityTypes::Global: ps << "Global"; break;
        case NontargetedAffinityTypes::RoundRobin: ps << "Round Robin"; break;
        case NontargetedAffinityTypes::LocalProcessor: ps << "Local Processor"; break;
    }
    return ps;
}