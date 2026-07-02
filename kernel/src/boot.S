# QuantumOS Kernel Bootstrap (x86_64)
#
# Multiboot v1 loaders (GRUB "multiboot", QEMU -kernel) enter at _start in
# 32-bit protected mode with paging off, EAX = 0x2BADB002 and EBX = the
# physical address of the multiboot info structure. This stub saves those,
# clears BSS, identity-maps the first 1 GB with 2 MB pages, switches to
# long mode, enables SSE (the resonance subsystem is compiled with -msse),
# and calls kernel_main(magic, info_addr).

#define BOOT_STACK_SIZE 16384

#define MB1_MAGIC 0x1BADB002
#define MB1_FLAGS 0x00000003        /* page-align modules + memory info */

/* Multiboot v1 header — must be in the first 8 KB of the image; link.ld
 * places .multiboot_header first. No AOUT kludge needed for ELF. */
.section .multiboot_header, "a"
.align 4
    .long MB1_MAGIC
    .long MB1_FLAGS
    .long -(MB1_MAGIC + MB1_FLAGS)

.section .text
.global _start
.extern kernel_main

# ============================================================================
# 32-bit entry (protected mode, flat segments, paging off)
# ============================================================================
.code32
_start:
    cli
    movl $boot_stack_top, %esp

    /* Save multiboot registers to .data (survives the BSS clear) */
    movl %eax, multiboot_magic
    movl %ebx, multiboot_info

    /* Clear BSS now — the boot page tables live there */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* Identity-map the first 1 GB: PML4[0] -> PDPT[0] -> PD with 512
     * 2 MB entries. Non-zero entries only at index 0; the rest are zero
     * from the BSS clear. */
    movl $boot_pdpt, %eax
    orl $0x3, %eax                  /* present | writable */
    movl %eax, boot_pml4

    movl $boot_pd, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt

    movl $boot_pd, %edi
    movl $0x83, %eax                /* present | writable | 2MB page */
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* Enable PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    /* Load the page tables */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* Enable long mode (EFER.LME) */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* Enable paging — CPU enters compatibility mode */
    movl %cr0, %eax
    orl $0x80000001, %eax           /* PG | PE */
    movl %eax, %cr0

    /* Jump into 64-bit code via the 64-bit GDT */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

# ============================================================================
# 64-bit entry
# ============================================================================
.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $boot_stack_top, %rsp
    movq %rsp, %rbp

    # Enable SSE — the resonance subsystem is compiled with -msse/-msse2
    # (double-precision math), so XMM instructions must be legal before
    # kernel_main runs.
    mov %cr0, %rax
    and $~(1 << 2), %rax           # clear CR0.EM (no x87 emulation)
    or $(1 << 1), %rax             # set CR0.MP (monitor coprocessor)
    mov %rax, %cr0
    mov %cr4, %rax
    or $(3 << 9), %rax             # set CR4.OSFXSR | CR4.OSXMMEXCPT
    mov %rax, %cr4

    # kernel_main(uint32_t magic, uint32_t info_addr)
    movl multiboot_magic, %edi
    movl multiboot_info, %esi
    call kernel_main

    # Should never return
    cli
.halt:
    hlt
    jmp .halt

# Boot panic routine — C ABI: message in %rdi
.global boot_panic
boot_panic:
    # Print panic message
    call early_console_write

    # Print newline
    mov $panic_newline, %rdi
    call early_console_write

    # Halt system
    cli
.panic_loop:
    hlt
    jmp .panic_loop

# Early console output — C ABI: string in %rdi
# Interrupts are disabled for the duration so strings stay atomic on the
# serial line (timer-tick logs would otherwise interleave mid-string).
.global early_console_write
early_console_write:
    pushfq
    cli
    push %rax
    push %rdi
    push %rsi
    push %rcx
    push %rdx

    mov %rdi, %rsi    # keep string pointer; strlen takes %rdi
    call strlen
    mov %rax, %rcx    # length
    test %rcx, %rcx
    jz .write_done

    # Write to COM1 (0x3F8), waiting for the transmit holding register
    # to drain before each byte (LSR bit 5) so bytes are not dropped
.write_loop:
    mov $0x3FD, %dx   # Line Status Register
.wait_thr:
    in %dx, %al
    test $0x20, %al   # THR empty?
    jz .wait_thr

    mov $0x3F8, %dx
    mov (%rsi), %al
    out %al, %dx
    inc %rsi
    loop .write_loop

.write_done:
    pop %rdx
    pop %rcx
    pop %rsi
    pop %rdi
    pop %rax
    popfq             # restore caller's IF
    ret

# Write hex value
.global early_console_write_hex
early_console_write_hex:
    push %rax
    push %rdi
    push %rsi
    push %rdx
    push %rcx

    mov %rdi, %rax    # value to print
    mov $hex_buffer, %rsi
    mov $16, %ecx     # 16 hex digits

.hex_loop:
    rol $4, %rax      # rotate left 4 bits
    mov %rax, %rdx
    and $0xF, %rdx    # get low nibble

    cmp $10, %rdx
    jl .digit
    add $('A' - 10), %rdx
    jmp .write_digit
.digit:
    add $'0', %rdx

.write_digit:
    mov %dl, (%rsi)
    inc %rsi
    loop .hex_loop

    # Null terminate
    movb $0, (%rsi)

    # Print the hex string
    mov $hex_buffer, %rdi
    call early_console_write

    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax
    ret

# ============================================================================
# Data
# ============================================================================
.section .data
multiboot_magic: .quad 0
multiboot_info: .quad 0
panic_newline: .asciz "\r\n*** KERNEL PANIC ***\r\n"
hex_buffer: .space 17  # 16 hex digits + null

# 64-bit GDT: null, code (L=1), data. The pointer layout works for both
# the 32-bit lgdt (word + dword) and 64-bit reloads (word + qword) since
# the GDT sits below 4 GB.
.align 8
gdt64:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

# ============================================================================
# BSS: boot stack and boot page tables
# ============================================================================
.section .bss
.align 4096
.global boot_pml4
.global boot_pdpt
.global boot_pd
boot_pml4:
    .space 4096
boot_pdpt:
    .space 4096
boot_pd:
    .space 4096
.align 16
boot_stack:
    .space BOOT_STACK_SIZE
boot_stack_top:

# Non-executable stack marker
.section .note.GNU-stack, "", @progbits
