/**
 * QuantumOS init — a real compiled-C user program, ELF-loaded into
 * ring 3 (no libc, no CRT: _start is the entry point).
 *
 * Proves the ELF loader end-to-end: this is built by gcc, linked at
 * USER_VBASE, embedded in the kernel image, and mapped segment-by-
 * segment into a private address space at boot. It talks to the kernel
 * only through int 0x80 syscalls.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Syscall numbers (mirror kernel/include/kernel/syscall.h) */
#define SYS_WRITE   1
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_EXIT    4
#define SYS_TICKS   5

static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
    return ret;
}

static inline long syscall0(long n) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static void write_str(const char *s) { syscall1(SYS_WRITE, (long)s); }
static long getpid(void)             { return syscall0(SYS_GETPID); }
static void yield(void)              { syscall0(SYS_YIELD); }
static long ticks(void)              { return syscall0(SYS_TICKS); }
static void exit_(long code)         { syscall1(SYS_EXIT, code); }

void _start(void) {
    write_str("init: ELF-loaded C program running in ring 3");

    /* Exercise a few syscalls */
    volatile long pid = getpid();
    (void)pid;
    write_str("init: obtained pid + tick count via syscalls");

    long t0 = ticks();
    for (int i = 0; i < 4; i++) {
        yield();
    }
    long t1 = ticks();
    if (t1 >= t0) {
        write_str("init: survived scheduler round-trips, exiting cleanly");
    }

    exit_(0);
    for (;;) { }   /* unreachable */
}
