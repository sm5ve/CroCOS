//
// Created by Spencer Martin on 2/18/25.
//

#include <stddef.h>

extern "C" void* memset(void* dest, int value, size_t len) {
    asm volatile (
            // --- Setup ---
            // rdi  <- dest pointer
            // al   <- fill byte (only low 8 bits used)
            // rcx  <- length in bytes
            "mov %[dest], %%rdi\n"         // rdi = dest
            "movb %[value], %%al\n"         // al = fill value (8-bit)
            "mov %[len], %%rcx\n"          // rcx = len

            // --- Short Fill for Small Lengths (< 8 bytes) ---
            "cmp $8, %%rcx\n"              // if len < 8,
            "jb 3f\n"                      //   jump to label 3 (simple byte fill)

            // --- Compute Alignment Offset ---
            // Calculate how many bytes until rdi becomes 8-byte aligned:
            //   offset = (-dest) & 7
            "mov %%rdi, %%rdx\n"           // rdx = dest
            "neg %%rdx\n"                  // rdx = -dest
            "and $7, %%rdx\n"              // rdx = (-dest) & 7
            "cmp %%rdx, %%rcx\n"           // if len < alignment offset...
            "jb 2f\n"                      //   jump to label 2 (fall back to byte fill)

            // --- Fill Unaligned Head ---
            "sub %%rdx, %%rcx\n"           // rcx = rcx - alignment_offset
            "rep stosb\n"                  // Fill alignment_offset bytes; rdi advances

            // --- Build 64-bit Fill Pattern ---
            // We want RAX = {value,value,...,value} (8 copies)
            "movzb %%al, %%rax\n"          // rax = zero-extended al (8-bit value in lower byte)
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $8, %%r8\n"               // r8 <<= 8
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 has two copies (16 bits)
            "mov %%r8, %%rax\n"            // rax = 16-bit pattern
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $16, %%r8\n"              // r8 <<= 16
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 has four copies (32 bits)
            "mov %%r8, %%rax\n"            // rax = 32-bit pattern
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $32, %%r8\n"              // r8 <<= 32
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 holds 8 copies (64 bits)
            "mov %%r8, %%rax\n"            // rax = final 64-bit fill pattern

            // --- Bulk 8-Byte Fill and Tail ---
            "mov %%rcx, %%rdx\n"           // rdx = remaining byte count after head fill
            "shr $3, %%rcx\n"              // rcx = number of 8-byte (qword) chunks (divide by 8)
            "and $7, %%rdx\n"              // rdx = remaining tail bytes (len mod 8)
            "rep stosq\n"                  // Fill rcx qwords using pattern in rax
            "mov %%rdx, %%rcx\n"           // rcx = tail byte count
            "rep stosb\n"                  // Fill the remaining tail bytes
            "jmp 4f\n"                     // Jump to end

            "2:\n"  // --- Not enough bytes to reach alignment: do simple fill ---
            "rep stosb\n"                  // Fill all rcx bytes with byte fill
            "jmp 4f\n"                     // Jump to end

            "3:\n"  // --- For len < 8, do simple byte fill ---
            "rep stosb\n"                  // Fill rcx bytes with byte fill

            "4:\n"  // --- End: Restore original dest for return ---
            "mov %[dest], %%rax\n"         // rax = original dest
            : //no outputs
            : [dest] "r" (dest), [value] "b" ((char)value), [len] "r" (len)
            : "memory", "rdi", "rax", "rcx", "rdx", "r8", "al"
            );
    return dest;
}

extern "C" void* memswap(void* dest, const void* src, size_t len) {
    asm volatile (
            // --- Setup ---
            // rdi  <- dest pointer
            // rsi  <- src pointer
            // rcx  <- length in bytes
            "mov %[dest], %%rdi\n"         // rdi = dest
            "mov %[src], %%rsi\n"          // rsi = src
            "mov %[len], %%rcx\n"          // rcx = len

            // --- Short Swap for Small Lengths (< 8 bytes) ---
            "cmp $8, %%rcx\n"              // if len < 8,
            "jb 3f\n"                      //   jump to label 3 (simple byte swap)

            // --- Compute Alignment Offset ---
            // Calculate how many bytes until rdi becomes 8-byte aligned:
            //   offset = (-dest) & 7
            "mov %%rdi, %%rdx\n"           // rdx = dest
            "neg %%rdx\n"                  // rdx = -dest
            "and $7, %%rdx\n"              // rdx = (-dest) & 7
            "cmp %%rdx, %%rcx\n"           // if len < alignment offset...
            "jb 2f\n"                      //   jump to label 2 (fall back to byte swap)

            // --- Swap Unaligned Head ---
            "sub %%rdx, %%rcx\n"           // rcx = rcx - alignment_offset
            "rep movsb\n"                  // Swap alignment_offset bytes between rdi and rsi

            // --- Build 64-bit Swap Pattern ---
            // We want RAX = {value,value,...,value} (8 copies) for swapping
            "movzb %%al, %%rax\n"          // rax = zero-extended al (8-bit value in lower byte)
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $8, %%r8\n"               // r8 <<= 8
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 has two copies (16 bits)
            "mov %%r8, %%rax\n"            // rax = 16-bit pattern
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $16, %%r8\n"              // r8 <<= 16
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 has four copies (32 bits)
            "mov %%r8, %%rax\n"            // rax = 32-bit pattern
            "mov %%rax, %%r8\n"            // r8 = rax
            "shl $32, %%r8\n"              // r8 <<= 32
            "or %%rax, %%r8\n"             // r8 |= rax  -> now r8 holds 8 copies (64 bits)
            "mov %%r8, %%rax\n"            // rax = final 64-bit pattern

            // --- Bulk 8-Byte Swap and Tail ---
            "mov %%rcx, %%rdx\n"           // rdx = remaining byte count after head swap
            "shr $3, %%rcx\n"              // rcx = number of 8-byte (qword) chunks (divide by 8)
            "and $7, %%rdx\n"              // rdx = remaining tail bytes (len mod 8)
            "rep movsq\n"                  // Swap rcx qwords between rdi and rsi using pattern in rax
            "mov %%rdx, %%rcx\n"           // rcx = tail byte count
            "rep movsb\n"                  // Swap the remaining tail bytes between rdi and rsi
            "jmp 4f\n"                     // Jump to end

            "2:\n"  // --- Not enough bytes to reach alignment: do simple swap ---
            "rep movsb\n"                  // Swap all rcx bytes between rdi and rsi
            "jmp 4f\n"                     // Jump to end

            "3:\n"  // --- For len < 8, do simple byte swap ---
            "rep movsb\n"                  // Swap rcx bytes between rdi and rsi

            "4:\n"  // --- End: Restore original dest for return ---
            "mov %[dest], %%rax\n"         // rax = original dest
            : //no outputs
            : [dest] "r" (dest), [src] "r" (src), [len] "r" (len)
            : "memory", "rdi", "rsi", "rax", "rcx", "rdx", "r8", "al"
            );
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, size_t len) {
    asm volatile (
            // --- Setup ---
            // rdi  <- dest pointer
            // rsi  <- src pointer
            // rcx  <- length in bytes
            "mov %[dest], %%rdi\n"         // rdi = dest
            "mov %[src], %%rsi\n"          // rsi = src
            "mov %[len], %%rcx\n"          // rcx = len

            // --- Short Copy for Small Lengths (< 8 bytes) ---
            "cmp $8, %%rcx\n"              // if len < 8,
            "jb 3f\n"                      //   jump to label 3 (simple byte copy)

            // --- Compute Alignment Offset ---
            // Calculate how many bytes until rdi becomes 8-byte aligned:
            //   offset = (-dest) & 7
            "mov %%rdi, %%rdx\n"           // rdx = dest
            "neg %%rdx\n"                  // rdx = -dest
            "and $7, %%rdx\n"              // rdx = (-dest) & 7
            "cmp %%rdx, %%rcx\n"           // if len < alignment offset...
            "jb 2f\n"                      //   jump to label 2 (fall back to byte copy)

            // --- Copy Unaligned Head ---
            "sub %%rdx, %%rcx\n"           // rcx = rcx - alignment_offset
            "rep movsb\n"                  // Copy alignment_offset bytes from src to dest

            // --- Bulk 8-Byte Copy ---
            // We want to copy 8-byte chunks at a time for speed
            "mov %%rcx, %%rdx\n"           // rdx = remaining byte count after head copy
            "shr $3, %%rcx\n"              // rcx = number of 8-byte (qword) chunks (divide by 8)
            "and $7, %%rdx\n"              // rdx = remaining tail bytes (len mod 8)
            "rep movsq\n"                  // Copy rcx qwords (8-byte chunks) from src to dest
            "mov %%rdx, %%rcx\n"           // rcx = tail byte count
            "rep movsb\n"                  // Copy the remaining tail bytes from src to dest
            "jmp 4f\n"                     // Jump to end

            "2:\n"  // --- Not enough bytes to reach alignment: do simple copy ---
            "rep movsb\n"                  // Copy all rcx bytes from src to dest
            "jmp 4f\n"                     // Jump to end

            "3:\n"  // --- For len < 8, do simple byte copy ---
            "rep movsb\n"                  // Copy rcx bytes from src to dest

            "4:\n"  // --- End: Restore original dest for return ---
            "mov %[dest], %%rax\n"         // rax = original dest
            : //no outputs
            : [dest] "r" (dest), [src] "r" (src), [len] "r" (len)
            : "memory", "rdi", "rsi", "rax", "rcx", "rdx"
            );
    return dest;
}