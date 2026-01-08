.section .trampoline, "awx"
.code16

.globl trampoline_template_start
.globl trampoline_template_end

trampoline_template_start:
    movw $0x3f8, %dx    # Port in DX
    movb $'x', %al      # Data in AL
1:
    outb %al, %dx       # Use DX, not immediate
    jmp 1b

trampoline_template_end: