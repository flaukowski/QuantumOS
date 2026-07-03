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
#define SYS_SVC_RESTARTS 9
#define SYS_QRAND     10
#define SYS_SEND_TO   11
#define SYS_COM2      12
#define SYS_QSEED     13

/* SYS_COM2 operations (arg 1) */
#define SYS_COM2_READ   0
#define SYS_COM2_WRITE  1

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
static inline long usys3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return r;
}

static inline void write_str(const char *s) { usys1(SYS_WRITE, (long)s); }
static inline long getpid(void)              { return usys0(SYS_GETPID); }
static inline void yield(void)               { usys0(SYS_YIELD); }
static inline long ticks(void)               { return usys0(SYS_TICKS); }
static inline void exit_(long code)          { usys1(SYS_EXIT, code); }
static inline void heartbeat(void)           { usys0(SYS_HEARTBEAT); }
/* Restart count of the calling process's service slot, or -1 if the
 * caller is not a registered service. Used to detect a watchdog rebirth. */
static inline long svc_restarts(void)         { return usys0(SYS_SVC_RESTARTS); }

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

/* Targeted send: transmit to a specific destination pid the caller holds an
 * IPC send-capability for. Returns 0 on success, negative on error (-4 EPERM
 * when no capability authorises sending to `dest`). Generalises send_msg
 * (first-match) so a service can reply to the sender recv_msg handed it. */
static inline long send_to(long dest, const char *msg, long len) {
    return usys3(SYS_SEND_TO, dest, (long)msg, len);
}

/* Fill buf with up to `len` bytes from the kernel quantum subsystem's
 * qseed-mixed generator. Returns the byte count on success, or a negative
 * errno (-4 EPERM when the caller holds no quantum-pool read capability). */
static inline long qrand_fill(void *buf, long len) {
    return usys2(SYS_QRAND, (long)buf, len);
}
/* Provenance query: 1 if the kernel booted with a qseed handoff, 0 if not,
 * negative if the caller lacks the quantum-pool capability. */
static inline long qrand_seed_present(void) {
    return usys2(SYS_QRAND, 0, 0);
}
/* The boot qseed value the kernel accepted (0 = none), or negative errno
 * (-4 EPERM) if the caller holds no quantum-pool read capability. */
static inline long qseed_value(void) {
    return usys0(SYS_QSEED);
}

/* Write `len` bytes to the COM2 swarm-bridge UART. Returns bytes written, or
 * -4 EPERM without a COM2 device write-capability. */
static inline long com2_write_bytes(const void *buf, long len) {
    return usys3(SYS_COM2, SYS_COM2_WRITE, (long)buf, len);
}
/* Read up to `len` bytes from COM2 (non-blocking: returns 0 if none waiting),
 * or -4 EPERM without a COM2 device read-capability. */
static inline long com2_read_bytes(void *buf, long len) {
    return usys3(SYS_COM2, SYS_COM2_READ, (long)buf, len);
}

/* Tiny string helpers (no libc) */
static inline long str_len(const char *s) {
    long n = 0;
    while (s[n]) n++;
    return n;
}

#endif /* USYS_H */
