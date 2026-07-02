# QuantumOS embedded user-mode programs
#
# Position-independent ring-3 blobs. The kernel memcpys the bytes
# between the start/end symbols into a user-accessible region and
# starts a PROCESS_TYPE_USER process at the region base. There is no
# ELF loader yet, so everything (code + strings) must be inside the
# copied range and addressed rip-relative.
#
# Syscall ABI (int 0x80): rax = number, rdi/rsi = args, result in rax.
#   1 = write(const char *msg)   2 = getpid()   3 = yield()
#   4 = exit(code)               5 = ticks()

.section .rodata

# ----------------------------------------------------------------------------
# hello: announce, yield through the scheduler a few times, exit(0)
# ----------------------------------------------------------------------------
.global user_hello_start
.global user_hello_end
.align 16
user_hello_start:
    lea 1f(%rip), %rdi     # msg
    mov $1, %rax           # SYS_WRITE
    int $0x80

    mov $3, %rbx           # yield 3 times
2:
    mov $3, %rax           # SYS_YIELD
    int $0x80
    dec %rbx
    jnz 2b

    lea 3f(%rip), %rdi
    mov $1, %rax           # SYS_WRITE
    int $0x80

    xor %rdi, %rdi
    mov $4, %rax           # SYS_EXIT(0)
    int $0x80
4:  jmp 4b                 # unreachable

1:  .asciz "hello from ring 3 — syscalls work"
3:  .asciz "user process done, exiting cleanly"
user_hello_end:

# ----------------------------------------------------------------------------
# rogue: announce, then read kernel memory. The kernel text lives on
# supervisor pages, so this must page-fault; the kernel should contain
# the fault by killing this process — not by panicking.
# ----------------------------------------------------------------------------
.global user_rogue_start
.global user_rogue_end
.align 16
user_rogue_start:
    lea 1f(%rip), %rdi
    mov $1, %rax           # SYS_WRITE
    int $0x80

    mov 0x100000, %rax     # kernel text @ 1MB — supervisor page -> #PF

    lea 2f(%rip), %rdi     # must never be reached
    mov $1, %rax
    int $0x80
    xor %rdi, %rdi
    mov $4, %rax
    int $0x80
3:  jmp 3b

1:  .asciz "rogue: about to touch kernel memory from ring 3"
2:  .asciz "rogue: SHOULD NOT PRINT — isolation broken"
user_rogue_end:

# Non-executable stack marker
.section .note.GNU-stack, "", @progbits
