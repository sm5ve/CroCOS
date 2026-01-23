#define ASM_FILE 1
#include <kconfig.h>
#include "multiboot.h"

//Multiboot 1 (on QEMU at least) doesn't know how to load 64-bit ELFs. Thus, we have to pass the
//MULTIBOOT_AOUT_KLUDGE flag and tell the bootloader where our sections are.
.set MULTIBOOT_HEADER_FLAGS, (MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_VIDEO_MODE | MULTIBOOT_AOUT_KLUDGE)

.section .multiboot
.code32
.align 4

.set MULTIBOOT_HEADER_SIZE, (multiboot_header_end - multiboot_header_start)

multiboot_header_start:

//standard multiboot 1 header

.long MULTIBOOT_HEADER_MAGIC
.long MULTIBOOT_HEADER_FLAGS
.long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

//Multiboot memory info

.extern phys_start
.extern phys_end
.extern bss_start

//Tell the bootloader where the sections are located in the binary
.long multiboot_header_start
.long phys_start
.long bss_start
.long phys_end
.long _start

#Multiboot video mode info

#ifdef FRAMEBUFFER
//mode type
.long 0x00000000
.long 1280 //height
.long 1024 //width
.long 32 //depth
#endif

multiboot_header_end:

.align 8
gdtr:
    .word gdt_end - gdt_start - 1
    .quad gdt_start
gdt_start:
//Null segment
.quad 0
//Code segment
.word 0xffff
.word 0
.byte 0
.byte 0x9A //Remember to give ourselves permission to execute in the code segment!
.byte 0x20 //Flag to enable long mode
.byte 0
//Data segment
.word 0xffff
.word 0
.byte 0
.byte 0x92
.byte 0
.byte 0
gdt_end:

.section .bss
.align 16

stack_bottom:
.skip KERNEL_STACK_SIZE
.global stack_top
stack_top:

.global mboot_magic
mboot_magic:
    .long 0
.global mboot_table
mboot_table:
    .long 0

#Allocate initial space for bootstrapping page tables/data structures

.align 4096
.global boot_pml4
boot_pml4:
    .skip 4096
.global boot_page_directory_pointer_table
boot_page_directory_pointer_table:
    .skip 4096
boot_page_directory:
    .skip 4096

.section .text.bootstrap
.global _start
.type _start @function
.extern setupBootstrapPaging
.code32
_start:
    #backup the multiboot information
    movl %eax, (mboot_magic - VMEM_OFFSET)
    movl %ebx, (mboot_table - VMEM_OFFSET)
    #zero out the tables pml4 - boot_page_directory2

    # Zero out the tables pml4 - boot_page_directory2
    lea (boot_pml4 - VMEM_OFFSET), %edi
    mov $0, %eax
    mov $4096, %ecx  # 4096 bytes = 512 entryBuffer * 8 bytes each
    zero_loop:
        mov %eax, (%edi)
        add $4, %edi
        loop zero_loop

    # Set the top and bottom entryBuffer in the PML4
    mov $(boot_page_directory_pointer_table - VMEM_OFFSET), %eax
    or $3, %eax
    mov %eax, (boot_pml4 - VMEM_OFFSET)
    mov %eax, (boot_pml4 + 511 * 8 - VMEM_OFFSET)

    # Set up page directory pointer table
    mov $(boot_page_directory - VMEM_OFFSET), %eax
    or $3, %eax
    mov %eax, (boot_page_directory_pointer_table - VMEM_OFFSET)
    mov %eax, (boot_page_directory_pointer_table + 511 * 8 - VMEM_OFFSET)

    # Populate page directories using 4 MiB pages
    mov $0, %eax      # Clear the base register
    mov $0, %edi      # Index for boot_page_directory1
    mov $0, %esi      # Index for boot_page_directory2
    mov $512, %ecx    # Loop counter

    populate_loop:
        # boot_page_directory1[i] = (i << 21) | (1 << 7) | 3
        mov %eax, %ebx
        shl $21, %ebx
        or $((1 << 7) | 3), %ebx
        mov %ebx, (boot_page_directory - VMEM_OFFSET)(,%eax,8)

        # Increment counter
        inc %eax
        loop populate_loop

    # Disable paging just in case
    mov %cr0, %ebx
    and $(~(1 << 31)), %ebx
    mov %ebx, %cr0

    # Load the base address of PML4 into CR3
    mov $(boot_pml4 - VMEM_OFFSET), %eax
    mov %eax, %cr3

    # Enable PAE (bit 5 in CR4)
    mov %cr4, %eax
    orl $(1 << 5), %eax
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    # Enable paging
    mov %cr0, %ebx
    orl $((1 << 31) | 1), %ebx
    mov %ebx, %cr0

    lgdt gdtr
    #I was gonna load the data segments, but when I do that my VM crashes for some reason.
    #But it works without setting them, so whatever

    #Now that we've set the long mode bit in the code segment of the GDT, let's do a long jump to load that
    #segment and officially enter long mode!
    ljmp $0x8,$realm64

realm64:
    #Guys, gals, and nonbinary pals: we're running 64 bit code
    .code64
    #So let's immediately get out of here and jump up to -2GB
    mov $higher_half, %rax
    jmp *%rax

.size _start, . - _start

.section .text
.extern kernel_main
higher_half:
    #Set up our stack in the right part of memory
    mov $stack_top, %rsp
    #Remember the information passed to us by multiboot
    movl mboot_magic, %eax
    movl mboot_table, %ebx
    #Call C++
    call kernel_main

	cli
1:	hlt
	jmp 1b