# Interrupt Routing System — Open Work

Remaining items after the initial cleanup pass. All confirmed bugs, API improvements, naming changes, and structural work from the original draft have been resolved. What remains is architectural.

---

## 1. Dynamic Topology Updates

### 1.1 Post-boot device registration (`updateRouting` must become re-entrant)

Today the topology is fully built during boot and `updateRouting()` is called once before any APs are running. There is no provision for adding new domains after the system is live.

**Goals:**
- Allow a device driver to register a new `InterruptDomain` and its connectors at any point after boot, then trigger a routing update.
- The routing system must re-run its policy and apply the new configuration without disrupting in-service interrupts.

**Design sketch:**
- Expose `topology::addDevice(SharedPtr<InterruptDomain>, SharedPtr<DomainConnector>)`, callable from any kernel context after boot (with appropriate locking).
- `updateRouting()` must atomically swap in the new `handlersByVector` and `eoiBehaviorTable` rather than resetting them in-place. The current in-place reset-and-repopulate approach has a data race against `dispatchInterrupt` on any other CPU (see TODO comment in `updateRouting`). The correct fix is to build replacement tables to a separate allocation, swap the live pointer with a memory barrier, then defer-free the old tables once all CPUs have quiesced (RCU-style).
- A `interrupts::notifyTopologyChanged()` call that marks routing dirty and schedules `updateRouting()` on a kernel work queue would cover the AML/PCI enumeration case.
- For userspace drivers: a privileged syscall/IPC letting a driver declare "I handle device X via GSI Y with activation type Z"; the kernel validates and registers on the driver's behalf.

This is a larger feature with concurrency implications. Design alongside the driver model.

---

## 2. SMP and Level-Triggered Dispatch

### 2.1 SMP routing is architecturally incomplete

The current model uses one `CPUInterruptVectorFile` and one `LAPIC`, treating all 8 CPUs as a single destination. The routing graph has no concept of which CPU receives a given interrupt.

The design already supports the fix: instantiate one `CPUInterruptVectorFile` and one `LAPICLocalDeviceRoutingDomain` per logical CPU. The IOAPIC destination field would be driven by which CPU vector file the routing graph selects, enabling proper per-CPU load balancing. Defer until per-CPU-data infrastructure is further along.

---

### 2.2 Level-triggered EOI and dispatch ordering

`dispatchInterrupt` issues EOI **before** calling handlers. For edge-triggered interrupts on a single CPU this is harmless (IF is cleared by the IDT entry so the LAPIC can't re-deliver mid-handler), but the ordering is wrong in two ways that matter:

1. **Level-triggered semantics**: For a level-triggered interrupt the correct sequence is: mask the source at the nearest controller (to prevent re-assertion on the still-active line), run the handler, then issue EOI in reverse topological order (source-closest domain last). Issuing EOI first on an unmasked level-triggered line immediately re-triggers the interrupt.

2. **SMP**: LAPIC EOI broadcasts to all IOAPICs. Once sent, another CPU can take the same vector before the first CPU's handler returns. At scale this creates the potential for a handler to run concurrently with itself on two CPUs sharing a vector.

The `EOIChain` infrastructure already sorts domains by topological order, so the mechanism is in place. What needs to change when implementing level-triggered support:

- Mask the firing receiver before entering the handler.
- Move the EOI chain invocation to **after** the handler loop.
- After EOI, unmask (or leave masked for a driver to unmask via `setReceiverMask` once it has serviced the device).
- The `TRIGGER_LEVEL` warning block in `dispatchInterrupt` is the placeholder for this logic.
