/**
 * QuantumOS user-space syscall wrappers (freestanding, no libc).
 * Shared by the embedded user programs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef USYS_H
#define USYS_H

#define SYS_WRITE   1
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_EXIT    4
#define SYS_TICKS   5
#define SYS_SEND      6
#define SYS_RECV      7
#define SYS_HEARTBEAT 8

static inline long usys0(long n) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}
static inline long usys1(long n, long a1) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1) : "memory");
    return r;
}
static inline long usys2(long n, long a1, long a2) {
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(n), "D"(a1), "S"(a2) : "memory");
    return r;
}

static inline void write_str(const char *s) { usys1(SYS_WRITE, (long)s); }
static inline long getpid(void)              { return usys0(SYS_GETPID); }
static inline void yield(void)               { usys0(SYS_YIELD); }
static inline long ticks(void)               { return usys0(SYS_TICKS); }
static inline void exit_(long code)          { usys1(SYS_EXIT, code); }
static inline void heartbeat(void)           { usys0(SYS_HEARTBEAT); }

/* Capability-routed send: goes to whatever destination this process
 * holds an IPC send-capability for. Returns 0 on success, negative on
 * error (e.g. -4 EPERM when no capability authorises it). */
static inline long send_msg(const char *msg, long len) {
    return usys2(SYS_SEND, (long)msg, len);
}
/* Receive into buf (up to len); returns sender pid, or 0 if empty. */
static inline long recv_msg(char *buf, long len) {
    return usys2(SYS_RECV, (long)buf, len);
}

/* Tiny string helpers (no libc) */
static inline long str_len(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n;
}

#endif /* USYS_H */
