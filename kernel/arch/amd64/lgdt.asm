.globl load_gdt
.type load_gdt, @function
load_gdt:
    lgdt (%rdi)                 # rdi points to GDT descriptor

    mov $0x10, %ax              # Kernel data segment selector
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    # Do a far jump by pushing segment and offset manually
    pushq $0x08                 # Kernel code segment selector
    leaq flush_cs(%rip), %rax
    pushq %rax
    lretq                       # Far return to reload CS (acts like ljmp)

flush_cs:
    ret