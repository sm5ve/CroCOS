//
// Created by Spencer Martin on 4/5/25.
//

#ifndef CROCOS_FLUSHPLANNER_H
#define CROCOS_FLUSHPLANNER_H

namespace kernel::mm{
    class FlushPlanner{
    public:
        //This is *never* to be used outside the page table manager. The only reason this is not private
        //and setting the PTM as a friend is to avoid awkward use of ifdefs when porting to other architectures
        //and sticking the PTM in other namespaces
        void _ptmInternal_setPreviousPlanner(FlushPlanner* p){
            previousPlanner = p;
        }
        //Same as above - this is *never* to be used outside the page table manager
        FlushPlanner* _ptmInternal_getPreviousPlanner(){
            return previousPlanner;
        }
    private:
        FlushPlanner* previousPlanner;
    };
}

#endif //CROCOS_FLUSHPLANNER_H
