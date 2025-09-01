#define ASM_FILE 1

.critical_isr_trampoline:
    cli
    cld
    # Save rax early to use as a scratch register
    pushq   %rax
    # Load CS into rax and extract CPL
    # CS is 32 bytes above current rsp (stack looks like ... "CS" "Old RIP" "Error Code" "Vector Index" "Saved RAX")
    movq    32(%rsp), %rax
    andq    $0x3, %rax    # Extract CPL
    cmpq    $0x0, %rax
    je      .entry_body
    swapgs
    jmp     .entry_body

.general_isr_trampoline:
    cli
    cld
    # Save rax early to use as a scratch register
    pushq   %rax
    # Load CS into rax and extract CPL
    # CS is 32 bytes above current rsp (stack looks like ... "CS" "Old RIP" "Error Code" "Vector Index" "Saved RAX")
    movq    32(%rsp), %rax
    andq    $0x3, %rax    # Extract CPL
    cmpq    $0x0, %rax
    je      .no_swapgs_general_body

    swapgs
.no_swapgs_general_body:
    sti
.entry_body:

    # Save general-purpose registers (callee and caller-saved)
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rdi
    pushq   %rsi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    # Set up first argument to C handler
    movq    %rsp, %rdi         # rdi = pointer to interrupt stack frame

    # Call the common C handler
    call    interrupt_common_handler

    # Restore all saved registers
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rsi
    popq    %rdi
    popq    %rdx
    popq    %rcx
    popq    %rbx

    # once again swapgs back if we are returning to ring3
    cli
    movq    16(%rsp), %rax
    andq    $0x3, %rax
    cmpq    $0x0, %rax
    je      .no_swapgs_end

    swapgs
.no_swapgs_end:
    sti
    popq %rax
    addq $0x10, %rsp #pop the error code before calling iretq
    iretq

.macro ISR_NOERR vector
.global isr_\vector
isr_\vector:
    pushq   $0               # Push dummy error code
    pushq   $\vector         # Push vector index
    jmp     .general_isr_trampoline
.endm

.macro ISR_WITHERR vector
.global isr_\vector
isr_\vector:
    pushq   $\vector         # Push vector index
    jmp     .general_isr_trampoline
.endm

.macro CRITICAL_ISR_NOERR vector
.global isr_\vector
isr_\vector:
    pushq   $0               # Push dummy error code
    pushq   $\vector         # Push vector index
    jmp     .critical_isr_trampoline
.endm

.macro CRITICAL_ISR_WITHERR vector
.global isr_\vector
isr_\vector:
    pushq   $\vector         # Push vector index
    jmp     .critical_isr_trampoline
.endm

# Exception vectors 0-31 may require special error code handling
ISR_NOERR            0 # Divide-by-zero
ISR_NOERR            1 # Debug
CRITICAL_ISR_NOERR   2 # NMI
ISR_NOERR            3 # Breakpoint
ISR_NOERR            4 # Overflow
ISR_NOERR            5 # BOUND Range Exceeded
ISR_NOERR            6 # Invalid Opcode
ISR_NOERR            7 # Device Not Available
CRITICAL_ISR_WITHERR 8 # Double Fault
ISR_NOERR            9 # Coprocessor Segment Overrun
ISR_WITHERR         10 # Invalid TSS
ISR_WITHERR         11 # Segment Not Present
ISR_WITHERR         12 # Stack-Segment Fault
ISR_WITHERR         13 # General Protection Fault
ISR_WITHERR         14 # Page Fault
ISR_NOERR           15 # Reserved
ISR_NOERR           16 # x87 Floating-Point Exception
ISR_WITHERR         17 # Alignment Check
CRITICAL_ISR_NOERR  18 # Machine Check
ISR_NOERR           19 # SIMD Floating-Point Exception
ISR_NOERR           20 # Virtualization Exception
ISR_NOERR           21 # Control Protection Exception
ISR_NOERR           22 # Reserved
ISR_NOERR           23 # Reserved
ISR_NOERR           24 # Reserved
ISR_NOERR           25 # Reserved
ISR_NOERR           26 # Reserved
ISR_NOERR           27 # Reserved
ISR_NOERR           28 # Hypervisor Injection
ISR_NOERR           29 # VMM Communication Exception
ISR_NOERR           30 # Security Exception
ISR_NOERR           31 # Reserved

/* IRQ and other vectors (32–255) all use ISR_NOERR */
.irp vec, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47, \
         48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63, \
         64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79, \
         80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95, \
         96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111, \
         112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127, \
         128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143, \
         144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159, \
         160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175, \
         176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191, \
         192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207, \
         208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223, \
         224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239, \
         240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
    ISR_NOERR \vec
.endr