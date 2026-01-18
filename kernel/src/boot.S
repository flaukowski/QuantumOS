# QuantumOS Kernel Bootstrap (x86_64)

.section .text
.global _start
.extern kernel_main

# Multiboot2 header
.align 8
multiboot_header:
    .long 0xe85250d6               # Multiboot2 magic
    .long 0                        # Architecture 0 (i386)
    .long multiboot_header_end - multiboot_header
    .long 0xe85250d6               # Checksum
    .long 0                        # Header tags start
    .long 8                        # Header tags end
    .long 0                        # End tag type
    .long 8                        # End tag size
multiboot_header_end:

# Kernel entry point
.align 16
_start:
    # Setup stack
    mov $boot_stack_top, %rsp
    mov $boot_stack_top, %rbp
    
    # Clear BSS section
    mov $__bss_start, %rdi
    mov $__bss_end, %rcx
    sub %rdi, %rcx
    xor %rax, %rax
    rep stosb
    
    # Save multiboot information
    mov %rdi, multiboot_magic    # magic in RDI
    mov %rsi, multiboot_info     # info in RSI
    
    # Call kernel main
    call kernel_main
    
    # Should never return
    cli
.halt:
    hlt
    jmp .halt

# Boot panic routine
.global boot_panic
boot_panic:
    # Save registers
    push %rax
    push %rbx
    push %rcx
    push %rdx
    
    # Print panic message
    mov %rdi, %rsi    # message pointer
    call early_console_write
    
    # Print newline
    mov $panic_newline, %rsi
    call early_console_write
    
    # Restore registers
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    
    # Halt system
    cli
.panic_loop:
    hlt
    jmp .panic_loop

# Early console output (simplified)
.global early_console_write
early_console_write:
    push %rax
    push %rdi
    push %rsi
    push %rdx
    
    mov %rsi, %rdi    # string pointer
    call strlen
    mov %rax, %rcx    # length
    
    # Write to COM1 port (0x3F8)
    mov $0x3F8, %dx
.write_loop:
    mov (%rsi), %al
    out %al, %dx
    inc %rsi
    loop .write_loop
    
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax
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
    rol %rax, 4       # rotate left 4 bits
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
    mov $0, (%rsi)
    
    # Print the hex string
    mov $hex_buffer, %rsi
    call early_console_write
    
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax
    ret

# Data section
.section .data
multiboot_magic: .quad 0
multiboot_info: .quad 0
panic_newline: .asciz "\r\n*** KERNEL PANIC ***\r\n"
hex_buffer: .space 17  # 16 hex digits + null

# Stack section
.section .bss
.align 16
boot_stack:
    .space BOOT_STACK_SIZE
boot_stack_top:
