#define ASM 1
#include <kconfig.h>

.section .trampoline
.code16

.globl trampoline_template_start
.globl trampoline_template_end
.globl smp_bringup_pml4

#define RELOCATED(x) (x - trampoline_template_start + SMP_TRAMPOLINE_START * 0x1000)

trampoline_template_start:
    cli
    lgdt RELOCATED(smp_gdtr32)      # Load GDT
    mov $0x00000011, %eax
    mov %eax, %cr0

    ljmp $0x8, $RELOCATED(pmode)   # Far jump to protected mode

.code32
pmode:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    # Load the base address of PML4 into CR3
    mov (RELOCATED(smp_bringup_pml4)), %eax
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

    lgdt RELOCATED(smp_gdtr64)

    ljmp $0x8, $RELOCATED(lmode_entry)


.code64
lmode_entry:
    #Guys, gals, and nonbinary pals: we're running 64 bit code
    .code64
    #So let's immediately get out of here and jump up to -2GB
    mov $higher_half, %rax
    jmp *%rax

.align 8
smp_gdtr32:
    .word smp_gdt_end32 - smp_gdt_start32 - 1
    .quad RELOCATED(smp_gdt_start32)

smp_gdt_start32:
    .quad 0                        # Null descriptor

    # Code segment (32-bit)
    .word 0xffff                   # Limit low
    .word 0                        # Base low
    .byte 0                        # Base middle
    .byte 0x9A                     # Access: present, DPL=0, code, executable, readable
    .byte 0xCF                     # Flags: granularity, 32-bit
    .byte 0                        # Base high

    # Data segment
    .word 0xffff
    .word 0
    .byte 0
    .byte 0x92                     # Access: present, DPL=0, data, writable
    .byte 0xCF                     # Flags: granularity, 32-bit
    .byte 0
smp_gdt_end32:
smp_gdtr64:
    .word smp_gdt_end64 - smp_gdt_start64 - 1
    .quad RELOCATED(smp_gdt_start64)
smp_gdt_start64:
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
smp_gdt_end64:
smp_bringup_pml4:
.long 012345
.long 012345
trampoline_template_end:

.globl smp_bringup_stack
smp_bringup_stack:
.long 0
.extern smpEntry

.section .text
higher_half:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov (smp_bringup_stack), %rsp

    call smpEntry
2:
    hlt
    jmp 2b