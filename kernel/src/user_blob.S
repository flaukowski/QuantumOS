# QuantumOS embedded user-mode programs
#
# Position-independent ring-3 blobs. The kernel copies the bytes
# between each start/end symbol into freshly allocated physical frames
# and maps them at USER_VBASE (0x40000000) in the spawning process's
# own address space; code addresses strings rip-relative. A fixed
# writable data page lives at 0x40080000 and the stack top at
# 0x40100000 — the SAME virtual addresses in every process, backed by
# different physical frames (per-process CR3).
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
# canary: prove per-process isolation. Write a pid-derived marker to a
# FIXED user VA (0x40080000), yield so the sibling instance writes ITS
# marker to the same VA, then read back. With per-process page tables
# each instance has its own frame there, so the marker survives; if the
# space were shared, the sibling's write would clobber it.
# ----------------------------------------------------------------------------
.global user_canary_start
.global user_canary_end
.align 16
user_canary_start:
    mov $2, %rax           # SYS_GETPID
    int $0x80
    add $0x1000, %rax      # marker = 0x1000 + pid
    mov %rax, %rbx         # keep our marker

    mov $0x40080000, %rdx  # fixed user data VA
    mov %rbx, (%rdx)       # store marker

    lea 1f(%rip), %rdi
    mov $1, %rax
    int $0x80

    mov $4, %rcx           # yield 4x — let the sibling store its marker
2:
    mov $3, %rax
    int $0x80
    dec %rcx
    jnz 2b

    mov $0x40080000, %rdx
    mov (%rdx), %rax       # read back
    cmp %rbx, %rax
    jne 9f                 # clobbered -> isolation broken

    lea 3f(%rip), %rdi     # marker intact
    mov $1, %rax
    int $0x80
    jmp 8f
9:
    lea 4f(%rip), %rdi     # marker clobbered
    mov $1, %rax
    int $0x80
8:
    xor %rdi, %rdi
    mov $4, %rax           # SYS_EXIT(0)
    int $0x80
7:  jmp 7b

1:  .asciz "canary: marker written to shared VA 0x40080000"
3:  .asciz "canary: marker intact after sibling ran — ISOLATION VERIFIED"
4:  .asciz "canary: marker CLOBBERED — isolation broken"
user_canary_end:

# ----------------------------------------------------------------------------
# rogue: announce, then read kernel memory. The kernel half is mapped
# in every process but supervisor-only, so a ring-3 read must page-fault
# and be contained (process killed, kernel survives).
# ----------------------------------------------------------------------------
.global user_rogue_start
.global user_rogue_end
.align 16
user_rogue_start:
    lea 1f(%rip), %rdi
    mov $1, %rax           # SYS_WRITE
    int $0x80

    mov $0x100000, %rdx    # kernel text @ 1MB (supervisor page)
    mov (%rdx), %rax       # ring-3 read -> #PF

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
