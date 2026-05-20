//
// Created by Spencer Martin on 5/20/26.
//
// Value type describing an occupancy state change (Empty / Partial / Full)
// driven by an allocation or free. Lives in LibAlloc because the concept is
// purely an allocator-state notion: allocators report it to their callers so
// upstream routing logic (e.g. NUMAPool deciding whether to surface a page
// back to a pool) can react to threshold crossings.
//
// The namespace stays as Core for now to avoid churn at every call site;
// future cleanup may move it under a LibAlloc:: namespace.
//

#ifndef CROCOS_OCCUPANCYTRANSITION_H
#define CROCOS_OCCUPANCYTRANSITION_H

#include <stdint.h>

namespace Core {

enum class OccupancyState : uint8_t { Empty, Partial, Full };

struct OccupancyTransition {
    OccupancyState before;
    OccupancyState after;

    [[nodiscard]] bool becameFull()      const { return before != OccupancyState::Full  && after == OccupancyState::Full; }
    [[nodiscard]] bool becameEmpty()     const { return before != OccupancyState::Empty && after == OccupancyState::Empty; }
    [[nodiscard]] bool becameAvailable() const { return before == OccupancyState::Full  && after != OccupancyState::Full; }
};

} // namespace Core

#endif //CROCOS_OCCUPANCYTRANSITION_H
