/**
 * QuantumOS user-space syscall wrappers (freestanding, no libc).
 * Shared by the embedded user programs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef USYS_H
#define USYS_H

#define SYS_WRITE 1
#define SYS_GETPID 2
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_TICKS 5
#define SYS_SEND 6
#define SYS_RECV 7
#define SYS_HEARTBEAT 8
#define SYS_SVC_RESTARTS 9
#define SYS_QRAND 10
#define SYS_SEND_TO 11
#define SYS_COM2 12
#define SYS_QSEED 13
#define SYS_FIELD_SNAPSHOT 14
#define SYS_CONS 15
#define SYS_SYSINFO 16
#define SYS_OPEN 17
#define SYS_READ 18
#define SYS_CLOSE 19
#define SYS_READDIR 20
#define SYS_SPAWN 21
#define SYS_WAITPID 22
#define SYS_FWRITE 23
#define SYS_UNLINK 24
#define SYS_SYNC 25
#define SYS_RESOLVE 26
#define SYS_UDP 27
#define SYS_TCP 28
#define SYS_IMPRINT 29
#define SYS_RECALL 30
#define SYS_FIELD_INFO 31

/* SYS_RESOLVE / SYS_UDP: still pending (poll again) / ring full. */
#define RESOLVE_WOULDBLOCK (-11)
#define UDP_WOULDBLOCK (-11)

/* SYS_UDP operations (arg 1) */
#define UDP_BIND 0
#define UDP_SENDTO 1
#define UDP_RECVFROM 2
#define UDP_CLOSE 3

/* SYS_TCP operations (arg 1) */
#define TCP_CONNECT 0
#define TCP_SEND 1
#define TCP_RECV 2
#define TCP_CLOSE 3
#define TCP_STATUS 4
#define TCP_LISTEN 5
#define TCP_ACCEPT 6

/* SYS_WAITPID: the target is still running. */
#define WAITPID_RUNNING 256

/* SYS_COM2 operations (arg 1) */
#define SYS_COM2_READ 0
#define SYS_COM2_WRITE 1

/* SYS_CONS operations (arg 1) */
#define SYS_CONS_READ 0
#define SYS_CONS_WRITE 1

/* SYS_SYSINFO operations (arg 1) */
#define SYSINFO_PS 0
#define SYSINFO_MEM 1
#define SYSINFO_TIME 2
#define SYSINFO_QUIET 3
#define SYSINFO_PEER 4 /* epic #97: packed peer= IP, 0 = none */

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
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2) : "memory");
    return r;
}
static inline long usys3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return r;
}

static inline void write_str(const char *s) {
    usys1(SYS_WRITE, (long)s);
}
static inline long getpid(void) {
    return usys0(SYS_GETPID);
}
static inline void yield(void) {
    usys0(SYS_YIELD);
}
static inline long ticks(void) {
    return usys0(SYS_TICKS);
}
static inline void exit_(long code) {
    usys1(SYS_EXIT, code);
}
static inline void heartbeat(void) {
    usys0(SYS_HEARTBEAT);
}
/* Restart count of the calling process's service slot, or -1 if the
 * caller is not a registered service. Used to detect a watchdog rebirth. */
static inline long svc_restarts(void) {
    return usys0(SYS_SVC_RESTARTS);
}

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

/* Publish a downsampled memory-field snapshot (up to FIELD_SNAP_BYTES signed
 * bytes) to the kernel's framebuffer visualization sink. Uncapped: it grants
 * no authority and is a no-op in effect unless a GRUB/ISO framebuffer is
 * present. Returns bytes stored. */
static inline long field_snapshot(const void *buf, long len) {
    return usys2(SYS_FIELD_SNAPSHOT, (long)buf, len);
}

/* Drain up to `len` buffered console input bytes (COM1 RX + PS/2 keyboard).
 * Non-blocking: returns 0 when nothing is waiting, or -4 EPERM without a
 * console device read-capability. */
static inline long cons_read(void *buf, long len) {
    return usys3(SYS_CONS, SYS_CONS_READ, (long)buf, len);
}
/* Write `len` raw bytes to the console (no "[user pid]" prefix). Returns
 * bytes written, or -4 EPERM without a console device write-capability. */
static inline long cons_write(const void *buf, long len) {
    return usys3(SYS_CONS, SYS_CONS_WRITE, (long)buf, len);
}
/* Copy kernel-formatted introspection text (SYSINFO_PS / SYSINFO_MEM) into
 * buf. Uncapped, read-only. Returns bytes copied. */
static inline long sysinfo(long op, void *buf, long len) {
    return usys3(SYS_SYSINFO, op, (long)buf, len);
}
/* 1 if the kernel booted `quiet` (a service should then suppress its
 * steady-state console logging), else 0. Uncapped, no buffer. */
static inline long sysinfo_quiet(void) {
    return usys3(SYS_SYSINFO, SYSINFO_QUIET, 0, 0);
}

/* SYS_OPEN flags (arg 2). Write flags require the filesystem-write
 * capability; writes go to the RAM overlay (which shadows the initrd)
 * and always append — O_TRUNC clears the file first. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 2
#define O_TRUNC 4

/* Open a file read-only (RAM overlay first, then the embedded initrd).
 * Returns a small non-negative fd, -6 ENOENT if the path names no file. */
static inline long open_(const char *path) {
    return usys2(SYS_OPEN, (long)path, O_RDONLY);
}
/* Open with explicit flags (write combos need O_WRONLY|O_CREAT and the
 * filesystem-write capability; -4 EPERM without). */
static inline long openf_(const char *path, long flags) {
    return usys2(SYS_OPEN, (long)path, flags);
}
/* Sequential read from an open initrd file. Returns bytes copied (0 = EOF),
 * -1 EINVAL on a bad fd. */
static inline long read_(long fd, void *buf, long len) {
    return usys3(SYS_READ, fd, (long)buf, len);
}
/* Release an fd table slot. */
static inline long close_(long fd) {
    return usys1(SYS_CLOSE, fd);
}
/* Kernel-formatted listing under `path` ("/" lists everything): one
 * "FS: <name> <size>" line per file — initrd rows first, then RAM
 * overlay rows tagged [ram]. Returns bytes copied. */
static inline long readdir_(const char *path, void *buf, long len) {
    return usys3(SYS_READDIR, (long)path, (long)buf, len);
}

/* Append to a write-opened fd. Returns bytes appended (may be short at
 * the per-file size cap), -1 EINVAL on a non-writable fd. */
static inline long fwrite_(long fd, const void *buf, long len) {
    return usys3(SYS_FWRITE, fd, (long)buf, len);
}
/* Remove a RAM-overlay file. -4 EPERM without the write capability,
 * -6 ENOENT if absent, -5 EIO while some process holds it open. */
static inline long unlink_(const char *path) {
    return usys1(SYS_UNLINK, (long)path);
}
/* Flush the RAM overlay to the persistence disk. Returns files flushed,
 * -4 EPERM without the write capability, -5 EIO with no disk. */
static inline long sync_(void) {
    return usys0(SYS_SYNC);
}

/* Resolve a hostname into ip[4]. Non-blocking request/poll: returns
 * RESOLVE_WOULDBLOCK (-11) while pending (poll again after a yield), 0
 * with ip filled on success, -4 EPERM without the network capability,
 * -5 EIO if there is no network or the lookup failed. */
static inline long resolve_(const char *host, unsigned char *ip) {
    return usys2(SYS_RESOLVE, (long)host, (long)ip);
}

/* SYS_UDP request block (epic #80). Layout MUST stay byte-identical to
 * the kernel's udp_req_k_t: 24 bytes, buf at offset 16, no padding.
 *   BIND:     in port (0 = ephemeral)          -> returns sock id
 *   SENDTO:   in sock, ip/port (dst), len, buf -> 0, or UDP_WOULDBLOCK
 *   RECVFROM: in sock, len (buf size), buf     -> byte count (out ip/
 *             port = sender, out len = count), or UDP_WOULDBLOCK
 *   CLOSE:    in sock                          -> 0
 * Errors: -4 EPERM (no network capability / not the socket's owner),
 * -1 EINVAL (bad sock/port/dst), -2 EFAULT, -5 EIO (no NIC). */
typedef struct {
    long sock;
    unsigned char ip[4];
    unsigned short port;
    unsigned short len;
    void *buf;
} udp_req_t;

static inline long udp_(long op, udp_req_t *req) {
    return usys2(SYS_UDP, op, (long)req);
}

/* SYS_TCP request block (epics #82 client, #98 server). Reuses the
 * udp_req_t layout (the single connection per side means the `sock`
 * field is unused).
 *   CONNECT:  in ip/port (dst)          -> 0 connected, -11 connecting
 *   SEND:     in buf, len (<= 1460)      -> bytes accepted, -11 tx busy
 *   RECV:     in buf, len (buf size)     -> byte count, 0 = EOF, -11 empty
 *   CLOSE:    (none)                     -> 0 closed, -11 closing
 *   STATUS:   (none)                     -> 0 established, -11 connecting
 *   LISTEN:   in port                    -> 0 armed, -11 arming/draining
 *   ACCEPT:   (none)                     -> 0 peer connected, -11 waiting,
 *                                           -5 recover: CLOSE + re-LISTEN
 * Errors: -4 EPERM (no network capability), -1 EINVAL, -2 EFAULT,
 * -5 EIO (refused/reset/timeout/no NIC). One connection per pid: a
 * listener may not CONNECT and a client may not LISTEN. */
typedef udp_req_t tcp_req_t;

static inline long tcp_(long op, tcp_req_t *req) {
    return usys2(SYS_TCP, op, (long)req);
}

/* SYS_IMPRINT / SYS_RECALL request blocks (epic #95): the kernel
 * holographic field. Layouts MUST stay byte-identical to the kernel's
 * field_*_k_t twins (76 / 76 / 136 bytes, all 4-byte members + byte
 * arrays, no padding). The capability must name EXACTLY `region`
 * (CAP_RESOURCE_FIELD, granted declaratively to services); a capless or
 * wrong-region caller gets -4 EPERM. IMPRINT returns the slot index;
 * an all-equal-bytes pattern is -1 EINVAL (no direction to resonate
 * along). RECALL returns 0 with out->n rankings (n=0 = nothing
 * resonates — success); the winner's stored bytes come back in
 * out->winner (the completion a noisy probe recovers). Retrieval
 * reinforcement only happens for callers who also hold the write right. */
#define FIELD_PAT_MAX 64
#define FIELD_RANK_MAX 8
#define FIELD_SLOTS 8
#define FIELD_PREVIEW 16
#define FIELD_ENERGY_MAX 0x7FFF /* Q15 1.0 (mirror of kernel field.h) */
#define FIELD_ENERGY_MIN 0x0800 /* Q15 floor: no zero-energy squatting */

typedef struct {
    unsigned int region;
    unsigned int len; /* 1..FIELD_PAT_MAX */
    int energy_q15;   /* 0 => kernel default (0x4000) */
    unsigned char pattern[FIELD_PAT_MAX];
} field_imprint_req_t;

typedef struct {
    unsigned int region;
    unsigned int len;
    unsigned int k; /* 1..FIELD_RANK_MAX rankings wanted */
    unsigned char probe[FIELD_PAT_MAX];
} field_recall_req_t;

typedef struct {
    unsigned int n;
    struct {
        unsigned int slot;
        int score_q15;
    } rank[FIELD_RANK_MAX];
    unsigned int winner_len;
    unsigned char winner[FIELD_PAT_MAX];
} field_recall_out_t;

static inline long imprint_(field_imprint_req_t *req) {
    return usys1(SYS_IMPRINT, (long)req);
}
static inline long recall_(field_recall_req_t *req, field_recall_out_t *out) {
    return usys2(SYS_RECALL, (long)req, (long)out);
}

/* SYS_FIELD_INFO read-only enumeration twins (epic #127 B1). Byte-identical to
 * the kernel field_*_info_k_t (40 / 332 bytes); _Static_asserted so any drift
 * fails BOTH builds. field_info_(region, out) returns 0, or -4 EPERM (capless /
 * wrong region), -14 EFAULT (bad out), -22 EINVAL (bad region). */
typedef struct {
    unsigned int slot;
    unsigned int len;
    int energy_q15;     /* stored importance, stable */
    int eff_energy_q15; /* after age decay, time-varying */
    unsigned int retrievals;
    unsigned int age_ticks; /* saturates at UINT32_MAX */
    unsigned char preview[FIELD_PREVIEW];
} field_slot_info_t;

typedef struct {
    unsigned int region;
    unsigned int live;
    unsigned int capacity;
    field_slot_info_t slots[FIELD_SLOTS];
} field_info_out_t;

_Static_assert(sizeof(field_slot_info_t) == 40, "field slot info twin ABI drift");
_Static_assert(sizeof(field_info_out_t) == 332, "field info out twin ABI drift");

static inline long field_info_(unsigned int region, field_info_out_t *out) {
    return usys2(SYS_FIELD_INFO, (long)region, (long)out);
}

/* Start an initrd ELF as a new ring-3 process. `cmd` is a command line:
 * the first whitespace-separated token names the initrd file to load, and
 * the whole token list becomes the new process's argv (argv[0] = the path).
 * Returns the new pid, or -4 EPERM without the spawn capability, -6 ENOENT
 * if the first token names no file. */
static inline long spawn_(const char *cmd) {
    return usys1(SYS_SPAWN, (long)cmd);
}
/* Poll a spawned process: exit code (0-255) once exited, WAITPID_RUNNING
 * (256) while it lives, -6 ENOENT for an unknown pid. */
static inline long waitpid_(long pid) {
    return usys1(SYS_WAITPID, pid);
}

/* ------------------------------------------------------------------ *
 * Argument vector ABI (epic #62). The kernel packs a spawned process's
 * argv into a read-only page at a fixed VA; a program reads it with
 * get_args(). This layout MUST stay byte-identical to the kernel side in
 * kernel/src/syscall.c (there is no shared header across the ring
 * boundary). A process spawned with no arguments sees argc == 0.
 * ------------------------------------------------------------------ */
#define USER_ARGS_VADDR 0x40090000UL
#define USER_ARGS_MAX 8        /* most argv entries carried */
#define USER_ARGS_STRBYTES 480 /* packed NUL-terminated arg strings */

typedef struct {
    int argc;
    unsigned argv_off[USER_ARGS_MAX]; /* byte offset of each arg within strings */
    char strings[USER_ARGS_STRBYTES];
} user_args_t;

/* Read this process's argument vector. Stores up to `max` pointers into
 * argv[] (each valid for the life of the process) and returns the true
 * argc, which may exceed `max` (only the first `max` are addressable). */
static inline int get_args(char **argv, int max) {
    const user_args_t *a = (const user_args_t *)USER_ARGS_VADDR;
    int argc = a->argc;
    int n = (argc < max) ? argc : max;
    for (int i = 0; i < n; i++) {
        argv[i] = (char *)a->strings + a->argv_off[i];
    }
    return argc;
}

/* Tiny string helpers (no libc) */
static inline long str_len(const char *s) {
    long n = 0;
    while (s[n])
        n++;
    return n;
}

#endif /* USYS_H */
