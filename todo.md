1. (✅) Add unit tests for Variant and Optional – specifically stress test move and copy semantics
2. (✅) Fix failing unit tests for InterruptGraph, figure out source of memory leaks
3. Implement proper kmalloc to replace temporary bump allocator
4. Unit test kmalloc with special attention to concurrency
5. Wire up APIC, CPU interrupt vectors, legacy IRQ lines, and maybe HPET to interrupt topology graph
6. Write initial basic RoutingPolicy. Unit test
7. Write methods to configure InterruptDomains according to resulting RoutingGraph
8. Allow registering interrupt handlers to interrupt sources, automatically determine final vector

---
At this point, we have a minimal implementation of the interrupt manager, we can begin to test it with real devices

---

9. Create driver for PIT
10. Implement timer manager, sleep()
11. SMP bring up
12. Unit test RingBuffer
13. Rewrite PageTableManager to use RingBuffer abstraction, try to abstract out hardware details to minimize code in /arch
14. Confirm functionality of new PageTableManager with one processor
15. Stress test under concurrency
16. Attempt to write unit tests for PageAllocator, PageTableManager if possible
17. Write higher level memory management abstractions – memory zones, regions, virtual address spaces, virtual address space allocators
18. Connect kmalloc to memory manager, finally get past the fixed heap size for kernel bring up

---

At this point, the core of the memory and interrupt managers are in place, and we can begin working on the higher level components

---

19. Begin work on basic scheduler, implement kernel tasks
20. Very basic syscalls
21. Write elf loader, get userspace binaries running
22. Write tar library for initrd system

---

At this point, I'm willing to call the project viable, and I will get to start making some very fun design decisions