//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"
#include <kernel.h>
#include <kconfig.h>
#include <core/str.h>
#include <acpi.h>
#include <assert.h>
#include <core/math.h>
#include "multiboot.h"
#include <arch/amd64/interrupts/LegacyPIC.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/timers/PIT.h>

extern uint32_t mboot_magic;
extern uint32_t mboot_table;

alignas(4096) uint64_t bootstrapPageDir[512];
extern volatile uint64_t boot_pml4[512];
extern volatile uint64_t boot_page_directory_pointer_table[512];

//The following describes the memory map layout for the early boot kernel
//before any memory allocation abstractions are online.
//The 1 GiB window is mapped by mapTemporary1GiBWindow, and
//the local/global pools will belong to the PageAllocator
//and are reserved/mapped in reservePageAllocatorBufferForRange
/*
 * ┌─0xffffffffffffffff
 * │
 * │ First 2 GiB
 * │ Physical memory
 * │ Mapped here
 * │
 * ├─0xffffffff80000000
 * │
 * │ 1 GiB movable
 * │ window
 * │
 * ├─0xffffffff40000000
 * │
 * │ 4 KiB page
 * │ directory for this
 * │ mapping
 * │
 * │ global page pool
 * │ local page pools
 * │ free maps
 * │ small page pools
 * │ small free maps
 * │
 * ├─0xffffffff00000000
 * │
 * │ 4 KiB page
 * │ directory for this
 * │ mapping
 * │
 * │ global page pool
 * │ local page pools
 * │ free maps
 * │ small page pools
 * │ small free maps
 * │
 * ├─0xfffffffec0000000
 * │
 * │ .
 * │ .
 * │
 * ├─0x0001000000000000
 * │
 * │  Userspace
 * │  eventually
 * │
 * └─0x0000000000000000
 */

extern uint32_t phys_end;

size_t archProcessorCount;

namespace kernel::amd64{

    void flushTLB(){
        asm volatile("mov %cr3, %rax\n"
                     "mov %rax, %cr3");
    }

    void unmapIdentity(){
        boot_pml4[0] = 0;
        boot_page_directory_pointer_table[0] = 0;
        boot_page_directory_pointer_table[1] = 0;
    }

    //Window MUST start at 2MiB aligned base
    void* mapTemporary1GiBWindow(mm::phys_addr base){
        assert((base.value & (kernel::mm::PageAllocator::bigPageSize - 1)) == 0, "Misaligned window");
        for(size_t x = 0; x < 512; x++){
            //Write 2MiB page address to page table entry, truncate appropriately
            bootstrapPageDir[x] = (base.value + x * kernel::mm::PageAllocator::bigPageSize) & (mm::PageAllocator::maxMemorySupported - 1);
            bootstrapPageDir[x] |= (1 << 7) | 3; //Indicate this is a big page, it's R/W and present
        }
        //add a reference to our page directory to the page directory pointer table
        boot_page_directory_pointer_table[509] = amd64::early_boot_virt_to_phys(mm::virt_addr(&bootstrapPageDir)).value | 3;
        return (void*)(amd64::early_boot_phys_to_virt(mm::phys_addr((void*)0)).value - (1 << 30));
    }

    void unmapTemporaryWindow(){
        boot_page_directory_pointer_table[509] = 0;
        flushTLB();
    }

    //highest entry in the page directory pointer table that is free
    //pdpt[511] and pdpt[510] are kernel memory
    //pdpt[509] is the 1 GiB window
    //so 508 is the highest we can go.
    uint16_t pdptIndex = 508;

    //For each memory range in multiboot's mmap, we need to map the top of the address space into virtual memory,
    //so we can reserve a buffer for the page allocator. This means first mapping the top of that range into
    //a temporary 1 GiB window, then setting up more permanent page tables in the top of that memory range,
    //and finally unmapping the temporary window. Most of this function just deals with computing
    //relevant offsets to make sure everything has the right alignment, and the tedium of setting up
    //page tables by hand.
    void* reservePageAllocatorBufferForRange(mm::phys_memory_range& range, size_t processor_count) {
        //Determine the space needed for the page allocator buffer along with the extra small page at the top
        //of the range to map it in.

        const size_t buffer_space_needed = roundUpToNearestMultiple(
                mm::PageAllocator::requestedBufferSizeForRange(range, processor_count),
                                                                      mm::PageAllocator::smallPageSize);

        const size_t paging_structure_space_needed = (2 + divideAndRoundUp(buffer_space_needed, (uint64_t)1 << 30))
                * mm::PageAllocator::smallPageSize;


        //If we need 512 MiB of space to store our buffers, then someone's trying to run this
        //kernel on a machine with at least 512 GiB of memory. Should that day ever come, we can
        //update the logic here to be smarter. But for now, I'm willing to error if we have that much memory
        assert(paging_structure_space_needed == mm::PageAllocator::smallPageSize * 3,
               "Memory range size is too big! We don't support more than 512 GiB of memory yet!");

        const size_t total_space_needed = buffer_space_needed + paging_structure_space_needed;
        //Find the largest small page aligned address that will let us fit all necessary buffer structures
        //and paging structures in the memory range
        const mm::phys_addr buffer_phys_base(roundDownToNearestMultiple(range.end.value - total_space_needed,
                                                                        mm::PageAllocator::smallPageSize));

        const mm::phys_addr paging_phys_base(roundDownToNearestMultiple(range.end.value -
                                                paging_structure_space_needed, mm::PageAllocator::smallPageSize));

        //Find the physical addresses of the page table and page directory we will construct.
        //Necessary to install these in the boot page table structure.
        //If we ever get a test machine with 1 TiB of memory, we will need to do this in a smarter way
        const mm::phys_addr page_dir_phys_addr(paging_phys_base.value);
        const mm::phys_addr page_tbl_lower_phys_addr(paging_phys_base.value + mm::PageAllocator::smallPageSize);
        const mm::phys_addr page_tbl_upper_phys_addr(paging_phys_base.value + 2 * mm::PageAllocator::smallPageSize);
        //Shrink the range so the buffer is properly reserved.
        range.end = buffer_phys_base;
        //If the range is too small, we should be skipping it
        assert(buffer_phys_base.value > range.start.value,
               "Tried to allocate buffer in insufficiently large range");
        //our window only maps big page aligned ranges, so align it!
        const mm::phys_addr window_phys_base(roundDownToNearestMultiple(paging_phys_base.value,
                                                                  mm::PageAllocator::bigPageSize));
        //but also we need to remember our offset into this differently aligned window
        const size_t paging_offset = paging_phys_base.value - window_phys_base.value;
        //map the relevant memory into our window
        void* window_virt_base = mapTemporary1GiBWindow(window_phys_base);
        flushTLB(); //very necessary on the second memory range we process (and all thereafter)
        void* page_structures_begin = (void*)((uint64_t)window_virt_base + paging_offset);
        //clear the buffer
        memset(page_structures_begin, 0, paging_structure_space_needed);
        //get pointers to the page table and page directory
        uint64_t* page_dir = (uint64_t*)((uint64_t)page_structures_begin);
        uint64_t* page_tbl_lower = (uint64_t*)((uint64_t)page_structures_begin + mm::PageAllocator::smallPageSize);
        uint64_t* page_tbl_upper = (uint64_t*)((uint64_t)page_structures_begin + 2 * mm::PageAllocator::smallPageSize);

        //First we populate the page_tbl_lower so the rest of our buffer is big paged aligned
        uint64_t page_addr = window_phys_base.value;
        //Keep track of how far into the page table we need to go to hit our first mapping
        size_t buffer_offset = 0;
        for(size_t page_tbl_index = 0; page_tbl_index < mm::PageAllocator::smallPagesPerBigPage; page_tbl_index++){
            if(page_addr < buffer_phys_base.value){
                page_tbl_lower[page_tbl_index] = 0;
                buffer_offset += mm::PageAllocator::smallPageSize;
            }
            else if(page_addr < buffer_phys_base.value + buffer_space_needed){
                page_tbl_lower[page_tbl_index] = page_addr;
                page_tbl_lower[page_tbl_index] |= 3; //Mark as present and R/W
            }
            else{
                //Don't map too much!
                page_tbl_lower[page_tbl_index] = 0;
            }
            page_addr += mm::PageAllocator::smallPageSize;
        }

        //Then we map big pages until the tail of our buffer fits in a single big page

        assert(page_addr % mm::PageAllocator::bigPageSize == 0, "How did we get misaligned?");
        size_t page_dir_index = 0;
        page_dir[page_dir_index] = page_tbl_lower_phys_addr.value;
        page_dir[page_dir_index] |=  3; //Mark page table as present and R/W
        page_dir_index++;
        for(; page_addr < buffer_phys_base.value + buffer_space_needed; page_dir_index++){
            page_dir[page_dir_index] = page_addr;
            page_dir[page_dir_index] |= (1 << 7) | 3; //Mark as big page, present, and R/W
            page_addr += mm::PageAllocator::bigPageSize;
        }
        page_dir[page_dir_index] = page_tbl_upper_phys_addr.value;
        page_dir[page_dir_index] |=  3; //Mark page table as present and R/W

        //Then we map the tail in page_tbl_upper using small pages
        for(size_t page_tbl_index = 0; page_tbl_index < mm::PageAllocator::smallPagesPerBigPage; page_tbl_index++){
            if(page_addr < buffer_phys_base.value + buffer_space_needed){
                page_tbl_upper[page_tbl_index] = page_addr;
                page_tbl_upper[page_tbl_index] |= 3; //Mark as present and R/W
            }
            else{
                page_tbl_upper[page_tbl_index] = 0;
            }
            page_addr += mm::PageAllocator::smallPageSize;
        }

        //Install the page directory in our boot_page_directory_pointer_table and decrement pdptIndex
        assert(page_dir_phys_addr.value % mm::PageAllocator::smallPageSize == 0, "misaligned page directory");
        boot_page_directory_pointer_table[pdptIndex] = page_dir_phys_addr.value | 3;

        //Compute the virtual address of the base of the buffer, then decrement pdptIndex
        //0xffffff8000000000 is the lowest virtual address mappable by the PDPT. It is -512GiB.
        uint64_t pdpt_vmem_base = 0xffffff8000000000;
        assert(pdptIndex > 0, "Too many memory regions!");
        //Make sure we cast everything in sight to a long, so there are no intermediate truncations
        uint64_t pd_vmem_base = pdpt_vmem_base + (1l << 30) * (uint64_t)pdptIndex;
        //The buffer doesn't necessarily lie on a big page aligned boundary, so we need to add back in the offset
        void* buffer_base_virt = (void*)(pd_vmem_base + buffer_offset);
        //decrement our pdpt index, so we don't overwrite our entry when we process the next memory range!
        pdptIndex--;

        //clear the buffer memory
        memset(buffer_base_virt, 0, buffer_space_needed);
        //Return the pointer we computed above
        return buffer_base_virt;
    }

    bool supportsFSGSBASE() {
        uint32_t eax, ebx, ecx, edx;
        eax = 0x07;
        ecx = 0;
        asm volatile("cpuid"
                : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "c"(ecx));
        return (ebx & (1 << 0)) != 0;
    }

    void enableFSGSBase(){
        //TODO implement a fallback method
        //really I ought to just do this with a proper GDT or whatever... but this should be enough
        //to get the rudiments of SMP working.
        temporaryHack(10, 11, 2025, "Implement a proper GDT");
        assert(supportsFSGSBASE(), "Your CPU doesn't support FSGSBASE");
        uint64_t cr4;
        asm volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 16);
        asm volatile ("mov %0, %%cr4" :: "r"(cr4));
    }

    const uint64_t kernel_code_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 1ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed
    const uint64_t kernel_data_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 0ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed

    __attribute__((aligned(16)))
    static uint64_t gdt[] = {
            0x0000000000000000, // Null descriptor
            kernel_code_descriptor,
            kernel_data_descriptor
    };

    struct {
        uint16_t size;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
            .size = sizeof(gdt) - 1,
            .base = (uint64_t)&gdt
    };

    extern "C" void load_gdt(void*);

    void temporaryPageFaultHandler(hal::InterruptFrame& frame) {
        kernel::klog << "Page fault at " << reinterpret_cast<void*>(frame.rip) << "\n";
        print_stacktrace(&frame.rbp);
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    }

    void initializeInterrupts(acpi::MADT& madt) {
        interrupts::init();
        cli();
        interrupts::disableLegacyPIC();
        hal::interrupts::platform::setupCPUInterruptVectorFile(INTERRUPT_VECTOR_COUNT);
        interrupts::setupAPICs(madt);
        const auto exceptionVectors = make_shared<interrupts::ExceptionVectorDomain>(INTERRUPT_VECTOR_RESERVE_SIZE);
        hal::interrupts::topology::registerDomain(exceptionVectors);
        const auto exceptionVectorConnector =
            make_shared<hal::interrupts::platform::AffineConnector>(exceptionVectors,
            hal::interrupts::platform::getCPUInterruptVectors(), INTERRUPT_VECTOR_RESERVE_START, 0, INTERRUPT_VECTOR_RESERVE_SIZE);
        hal::interrupts::topology::registerExclusiveConnector(exceptionVectorConnector);

        using namespace hal::interrupts::managed;
        registerHandler(InterruptSourceHandle(exceptionVectors, 14), make_unique<InterruptHandler>(temporaryPageFaultHandler));
    }

    void hwinit(){
        assert(mboot_magic == 0x2BADB002, "Somehow the multiboot magic number is wrong. How did we get here?");

        unmapIdentity();
        load_gdt(&gdtr);
        enableFSGSBase();
        smp::setLogicalProcessorID(0);

        flushTLB();
        //Our first goal is to find the MADT, so we will have the right info to properly init the
        //page allocator.
        kernel::acpi::tryFindACPI();
        auto& madt = kernel::acpi::the<acpi::MADT>();

        archProcessorCount = madt.getEnabledProcessorCount();
        assert(archProcessorCount > 0, "MADT has no LAPIC entry");

        kernel::klog << "Processor count: " << archProcessorCount << "\n";

        Vector<mm::PageAllocator::page_allocator_range_info> free_memory_regions;

        mboot_info* mbootInfo = amd64::early_boot_phys_to_virt(mm::phys_addr(mboot_table)).as_ptr<mboot_info>();
        mboot_mmap_entry* mbootMmapBase = amd64::early_boot_phys_to_virt(mm::phys_addr(mbootInfo -> mmap_ptr)).as_ptr<mboot_mmap_entry>();

        //Extract a list of free memory regions from the multiboot header, reserve the buffers requested by
        //the page allocator, and package that up to pass along to the page allocator
        //TODO make sure we double-check that the LAPIC/IOAPIC address ranges are completely reserved
        for(mboot_mmap_entry* mbootMmapEntry = mbootMmapBase;
            (uint64_t)mbootMmapEntry < (uint64_t)mbootMmapBase + mbootInfo -> mmap_len; mbootMmapEntry++){
            if(mbootMmapEntry -> type == 0x1){ //If the memory region is free, we'll add it to the list!
                mm::phys_memory_range range = {mm::phys_addr(mbootMmapEntry -> addr),
                                               mm::phys_addr(mbootMmapEntry -> addr + mbootMmapEntry -> len)};
                if(range.getSize() > (mm::PageAllocator::bigPageSize * 2)){
                    uint64_t* buff = (uint64_t*)reservePageAllocatorBufferForRange(range, archProcessorCount);
                    free_memory_regions.push({range, buff});
                }
            }
        }

        unmapTemporaryWindow();

        kernel::mm::PageAllocator::init(free_memory_regions, archProcessorCount);
        //Find the memory range where the kernel resides and reserve it so we don't overwrite anything!
        mm::phys_memory_range range{.start=mm::phys_addr(nullptr), .end=mm::phys_addr(&phys_end)};
        kernel::mm::PageAllocator::reservePhysicalRange(range);

        kernel::klog << "Finished initializing page allocator\n";

        kernel::amd64::PageTableManager::init(archProcessorCount);

        klog << "Finished initializing page table manager\n";

        initializeInterrupts(madt);
        timers::initPIT();

        klog << "Finished initializing interrupts\n";
        //kernel::amd64::interrupts::buildApicTopology(madt); //Temporary ACPI initialization stuff...

        //kernel::amd64::PageTableManager::runSillyTest();
    }
}