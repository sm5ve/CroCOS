//
// RCU Phase 2 — test-only window into a kernel::rcu::Domain.
//
// P2-DEC-009 puts the engine in opaque storage inside Domain, which is exactly
// the property the phase wants (no consumer TU instantiates the Core template)
// and exactly the property that makes the domain untestable from outside: a
// test cannot name EpochDomain<KernelRcuHooks>, so it cannot reach
// Core::rcu::DebugIntrospection either. This header is the veneer's own thin
// forwarding layer to that one, following vmsmalloc's Phase-8 pattern
// (kernel/mm/vmsmalloc.cpp:537-564): declared here, defined inside RCU.cpp where
// the engine is visible, and gated so the kernel build never sees it.
//
// drainAllQuiescent WAS exposed here and nowhere else: RCU-DEC-035 gives it a
// precondition — an external guarantee that no CPU will touch the domain again —
// that Phase 2 had no kernel path to supply, dynamic domain teardown being a
// Non-Goal at the time. **RCU-DEC-043 (2026-08-07) retired that Non-Goal and
// added the veneer** (`kernel::rcu::drainAllQuiescent`), its precondition
// supplied by the radix consumer's thread-destruction step.
//
// This forwarder stays because it is what the existing tests call and because it
// is reachable on an uninitialized domain, which the veneer deliberately asserts
// against. The tests satisfy the precondition at the end of a joined single- or
// multi-threaded scenario, and they need it: without a teardown drain every test
// would leave its retirees in limbo and the residue assertions would be
// measuring the previous test's garbage.
//
// SNAPSHOTS ASSUME A QUIESCENT DOMAIN, exactly as the Core one does — ordinary
// loads with no protocol participation. Taken while other threads are live they
// are a torn view, not a measurement.
//

#ifndef CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H
#define CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H

#include <stddef.h>
#include <stdint.h>
#include <rcu/RCU.h>

#ifdef CROCOS_RCU_TEST_HARNESS

namespace kernel::rcu::test {

    // Is the owner of `slot` inside a read-side section? Meaningful only when
    // asked by that slot's own owner (the field is owner-only plain state).
    bool inSection(const Domain& d, size_t slot);

    // Nesting depth of the calling CPU's section on this domain.
    uint64_t nesting(const Domain& d, size_t slot);

    uint64_t epoch(const Domain& d);
    size_t   slotCount(const Domain& d);
    size_t   drainBatchBound(const Domain& d);

    // Nodes still in limbo across every slot and every bag. NOTE the correct
    // expectation is usually NOT zero: open-bag contents are recoverable only by
    // their owner (I13), so a slot that retires and then goes idle leaves its own
    // Open bag behind by design (ITEM-014). A test asserting this reaches zero
    // without first forcing a seal fails on a CORRECT build.
    size_t totalResidue(const Domain& d);

    // Nodes in slot i's currently-Open bag — the residue floor for an idle slot.
    size_t openBagResidue(const Domain& d, size_t slot);

    // RCU-DEC-034's quiescence check, invoked directly. Throws under the test
    // harness exactly where a debug kernel would panic.
    void assertQuiescent(const Domain& d);

    // RCU-DEC-035. Teardown drain under the caller's no-new-users guarantee;
    // force-seals remote Open bags. Engine-only in Phase 2 — see the file header.
    size_t drainAllQuiescent(Domain& d);
    // The domain's slot block, for the RCU-DEC-043 freelist-recycling test:
    // it asserts the SAME storage comes back, not merely that nothing ran out.
    const void* slotAddress(const Domain& d);
    // Drop the RCU-DEC-043 block freelist. The harness re-mmaps its arena per
    // test, so a block recycled by the previous test dangles into unmapped
    // memory; the kernel's window lives as long as the kernel and never needs
    // this. Every harness that constructs domains must call it at teardown.
    void resetDomainManagementState();

}   // namespace kernel::rcu::test

#endif // CROCOS_RCU_TEST_HARNESS

#endif // CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H
