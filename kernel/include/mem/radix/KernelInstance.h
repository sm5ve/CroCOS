//
// radix-tree Phase 5 — the kernel instance: base policy, codecs, and the type
// aliases everything kernel-side spells (§5.1, §10; DEC-019 / 090 / 093).
//
// ─── Why this is a separate header ────────────────────────────────────────
//
// `SlotCodec.h` ships the codec TEMPLATE and the harness's base policy, and it
// has to stay includable by the userspace harness, which has no `kmemlayout.h`
// and no page tables. The kernel base policy is derived from the VMSubstrate
// window's position in the kernel VA layout, so it cannot live there without
// dragging that dependency across the seam.
//
// So: the arithmetic is shared (both instances run the SAME shift-and-mask code
// — see SlotCodec.h's note on why a "literally uncompressed" harness codec
// cannot exist), and only the base and the window limit differ. §10's narrowed
// blind spot is exactly those two, which is what the `bindAndVerify` check below
// is for.
//
// ─── The one thing §10 asks of a codec instance ───────────────────────────
//
// "Each codec must assert its own base and range at construction rather than
// trust vas-layout.md to stay true."
//
// The base here is a `constexpr` derived from `VMM_SUBSTRATE_ROOT_INDEX` and the
// page-table geometry — the same expression `VMSubstrate::arenaVirtualBase(0)`
// evaluates — and the two derivations are independent enough to be worth
// checking against each other once, at stress/VMM start-up, rather than assuming
// they will stay in step. A drift here does not fault: it encodes slot words
// against the wrong base, and every decoded child pointer names memory the tree
// does not own.
//

#ifndef CROCOS_RADIX_KERNEL_INSTANCE_H
#define CROCOS_RADIX_KERNEL_INSTANCE_H

#include <stdint.h>
#include <arch.h>
#include <kassert.h>
#include <kmemlayout.h>
#include <mem/VMSubstrate.h>

#include <mem/radix/AddressSpace.h>
#include <mem/radix/BucketCodec.h>
#include <mem/radix/ClusterTable.h>
#include <mem/radix/CoreTree.h>
#include <mem/radix/DescentCache.h>
#include <mem/radix/Geometry.h>
#include <mem/radix/Placement.h>
#include <mem/radix/SlotCodec.h>

namespace kernel::mm::radix {

    // The VMSubstrate window's base VA, `constexpr`. Every node the tree
    // allocates comes from vmsmalloc, which lives inside this window, so a slot
    // word's compressed pointer field is an offset from here.
    //
    // Written as the same expression `VMSubstrate::arenaVirtualBase(0)`
    // evaluates rather than by calling it: the base is on the descent's decode
    // path, and an out-of-line call per slot would be one call per level per
    // fault. `verifyKernelCodecBase()` is what keeps the duplication honest.
    struct KernelSlotBase {
        static constexpr uintptr_t kBase =
            static_cast<uintptr_t>(
                arch::pageTableDescriptor.canonicalizeVirtualAddress(
                    virt_addr{static_cast<uint64_t>(VMM_SUBSTRATE_ROOT_INDEX)
                              << arch::pageTableDescriptor.getVirtualAddressBitCount(
                                     pageTableLevelForKMemRegion() - 1)}).value);

        // The whole window, unlike the harness's arena extent: in the kernel the
        // nominal window really is the range vmsmalloc can hand out.
        static constexpr uint64_t kLimit = uint64_t{1} << VMSubstrate::kWindowAddressBits;

        static constexpr uintptr_t base()        { return kBase; }
        static constexpr uint64_t  windowLimit() { return kLimit; }

        static_assert(kBase % kSlotPointerAlignment == 0,
                      "the VMSubstrate window base must be slot-pointer aligned, or the "
                      "codec's low bits collide with the tag");
    };

    // §10's construction-time check, run once by whoever brings the tree up.
    // Cheap, and the failure it catches is silent: a base that has drifted from
    // the substrate's own encodes and decodes perfectly while naming the wrong
    // memory.
    inline void verifyKernelCodecBase() {
        assert(KernelSlotBase::base() ==
                   static_cast<uintptr_t>(VMSubstrate::arenaVirtualBase(0).value),
               "radix: the kernel codec's window base has drifted from "
               "VMSubstrate::arenaVirtualBase(0) — every slot word would name the wrong "
               "memory, silently (§10)");
    }

    // ─── The kernel instance ───────────────────────────────────────────────
    //
    // DEC-090 fixes the default geometry and DEC-093 makes it a per-architecture
    // constexpr instance, so this is a naming exercise rather than a decision.
    // Spelled once, here, so nothing kernel-side re-derives the template
    // argument list and gets one of them wrong.
    template <GeometryDescriptor G>
    using KernelSlotCodec = SlotCodec<G, KernelSlotBase>;

    using KernelCodec   = KernelSlotCodec<kAmd64Geometry>;
    using KernelTree    = CoreTree<kAmd64Geometry, KernelCodec>;
    using KernelTable   = ClusterTable<kAmd64Geometry, KernelCodec>;
    using KernelBlock   = ControlBlock<kAmd64Geometry, KernelCodec>;
    using KernelBlocks  = ControlBlockFreelist<kAmd64Geometry, KernelCodec>;
    using KernelCache   = DescentCache<kAmd64Geometry, KernelCodec>;
    using KernelPin     = PinnedNode<kAmd64Geometry>;

}  // namespace kernel::mm::radix

#endif  // CROCOS_RADIX_KERNEL_INSTANCE_H
