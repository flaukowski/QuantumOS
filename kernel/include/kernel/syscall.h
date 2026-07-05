/**
 * QuantumOS System Call Interface
 *
 * Ring-3 entry into the kernel via int 0x80 (IDT gate with DPL 3,
 * dispatched through the existing interrupt path). rax selects the
 * call, rdi/rsi carry arguments, the result returns in rax.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

#define SYSCALL_VECTOR 0x80

/* Syscall numbers */
#define SYS_WRITE 1 /* rdi = NUL-terminated string in user memory */
#define SYS_GETPID 2
#define SYS_YIELD 3
#define SYS_EXIT 4 /* rdi = exit code */
#define SYS_TICKS 5
#define SYS_SEND 6         /* rdi = msg ptr, rsi = len; routed by IPC capability */
#define SYS_RECV 7         /* rdi = buf ptr, rsi = len; returns sender pid (0 = empty) */
#define SYS_HEARTBEAT 8    /* report liveness to the service watchdog */
#define SYS_SVC_RESTARTS 9 /* restart count of the caller's service slot, or -1 */
#define SYS_QRAND                                                                                  \
    10 /* rdi = buf, rsi = len; fill up to QRAND_MAX_BYTES bytes
                         * from the qseed-mixed quantum generator. Gated on a
                         * CAP_RESOURCE_QUANTUM read capability (EPERM without).
                         * len == 0 is a provenance query: returns 1 if the
                         * kernel booted with a qseed, else 0. */
#define SYS_SEND_TO                                                                                \
    11 /* rdi = dest pid, rsi = msg ptr, rdx = len; like
                         * SYS_SEND but to a chosen destination the caller holds
                         * an IPC send-capability for (EPERM if it holds none for
                         * that dest). Lets a service reply to the sender pid
                         * recv handed it rather than a first-match peer. */
#define SYS_COM2                                                                                   \
    12 /* rdi = op (SYS_COM2_READ/WRITE), rsi = buf, rdx = len;
                         * raw byte pipe on the COM2 swarm-bridge UART (0x2F8).
                         * Gated on a CAP_RESOURCE_DEVICE capability over
                         * DEVICE_ID_COM2 (CAP_WRITE to send, CAP_READ to recv);
                         * EPERM without. Returns bytes moved (read may be 0 when
                         * nothing is waiting — the pipe is non-blocking). */
#define SYS_QSEED                                                                                  \
    13 /* returns the u64 boot qseed the kernel accepted on the
                         * cmdline (0 = none). Gated on the same quantum-pool read
                         * capability as SYS_QRAND (EPERM without) — lets an
                         * attestation service name the entropy the system booted
                         * with, so a host can bind the boot to that qseed. */
#define SYS_FIELD_SNAPSHOT                                                                         \
    14 /* rdi = buf, rsi = len; publish up to
                         * FIELD_SNAP_BYTES signed bytes of a downsampled
                         * memory-field snapshot into a kernel visualization
                         * buffer. No capability: it grants no authority — it is
                         * a pure display sink the framebuffer renders under a
                         * GRUB/ISO boot. A no-op in effect under the -kernel/VGA
                         * path (nothing samples it). Returns bytes stored. */

#define SYS_CONS                                                                                   \
    15 /* rdi = op (SYS_CONS_READ/WRITE), rsi = buf, rdx = len;
                         * raw byte pipe on the interactive console (COM1 RX ring
                         * + PS/2 keyboard in, raw COM1 TX out — no "[user pid]"
                         * prefix, so a shell owns its own line discipline).
                         * Gated on a CAP_RESOURCE_DEVICE capability over
                         * DEVICE_ID_CONSOLE (CAP_READ to read, CAP_WRITE to
                         * write); EPERM without. Read is non-blocking: returns
                         * the bytes buffered so far (0 if none). */
#define SYS_SYSINFO                                                                                \
    16 /* rdi = op (SYSINFO_PS/MEM), rsi = buf, rdx = len;
                         * kernel-formatted live introspection text copied into
                         * the caller's buffer: the process table (one "PS: pid
                         * name STATE" line per live process) or memory stats
                         * ("MEM: heap free=... frames free=.../..."). Read-only
                         * and uncapped, like SYS_GETPID/SYS_TICKS: it names no
                         * authority, it only reports. Returns bytes copied. */

#define SYS_OPEN                                                                                   \
    17 /* rdi = path ptr (NUL-terminated, user memory); open a
                         * regular file on the read-only embedded initrd. Returns
                         * a small non-negative fd, ENOENT if the path names no
                         * file, EIO if the caller's fd table is full. Uncapped:
                         * the initrd is public read-only data baked into the
                         * kernel image (per-file capabilities are future work,
                         * noted in docs/INITRD_VFS.md). */
#define SYS_READ                                                                                   \
    18               /* rdi = fd, rsi = buf, rdx = len; sequential read from an
                         * open initrd file. Returns bytes copied (0 = EOF),
                         * EINVAL on a bad fd. */
#define SYS_CLOSE 19 /* rdi = fd; release the fd table slot. */
#define SYS_READDIR                                                                                \
    20 /* rdi = path ptr, rsi = buf, rdx = len; kernel-formatted
                         * listing of initrd files under path ("/" lists all):
                         * one "FS: <name> <size>" line per file. Returns bytes
                         * copied. */
#define SYS_SPAWN                                                                                  \
    21 /* rdi = path ptr (NUL-terminated, user memory); load the
                         * named initrd ELF into a fresh address space and start
                         * it as a ring-3 process. Returns the new pid. Spawning
                         * is REAL authority — gated on a CAP_RESOURCE_PROCESS
                         * capability carrying CAP_EXECUTE over SPAWN_RESOURCE_ID
                         * (EPERM without); only qsh is granted it. ENOENT if the
                         * path names no file, EIO if the load fails. */
#define SYS_WAITPID                                                                                \
    22 /* rdi = target pid; poll a spawned process's fate. Returns
                         * the exit code (0-255) once it has exited (served from
                         * a small kernel exit ledger, so a reaped PCB still
                         * answers), WAITPID_RUNNING (256) while it lives, ENOENT
                         * if the pid is unknown. Non-blocking: the caller loops
                         * with SYS_YIELD. Uncapped introspection, like
                         * SYS_SYSINFO. */

#define SYS_FWRITE                                                                                 \
    23 /* rdi = fd, rsi = buf, rdx = len; append to a file opened
                         * with write flags on the RAM overlay (epic #71). The
                         * write authority was checked at open; the fd carries
                         * it. Returns bytes appended (may be short at the file
                         * size cap), EINVAL on a non-writable or bad fd. */
#define SYS_UNLINK                                                                                 \
    24 /* rdi = path ptr; remove a RAM-overlay file and free its
                         * storage. Gated on the filesystem-write capability
                         * (EPERM without). Refused with EIO while any live
                         * process holds the file open; ENOENT if absent (the
                         * initrd is immutable — not unlinkable). */
#define SYS_SYNC                                                                                   \
    25 /* no args; serialize the RAM overlay to the ATA disk as a
                         * ustar archive behind the QDSK superblock (restored at
                         * next boot). Gated on the filesystem-write capability.
                         * Returns files flushed, EIO with no disk or on write
                         * failure. */

/* SYS_WAITPID: the target is still running. */
#define WAITPID_RUNNING 256

/* Resource id of the spawn right under CAP_RESOURCE_PROCESS. */
#define SPAWN_RESOURCE_ID 0

/* SYS_OPEN flags (rsi). Write flags require the filesystem-write
 * capability (CAP_RESOURCE_DEVICE over DEVICE_ID_DISK) — volume-level
 * write authority granted declaratively to qsh alone. O_WRONLY requires
 * O_CREAT (overlay files are created-or-appended; the initrd is
 * immutable). Writes always append; O_TRUNC clears the file first. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 2
#define O_TRUNC 4

/* Bytes SYS_FIELD_SNAPSHOT stores at most (bounds the kernel viz buffer). */
#define FIELD_SNAP_BYTES 256

/* Longest path SYS_OPEN/SYS_READDIR will copy from user memory. */
#define VFS_PATH_MAX 128

/* SYS_COM2 operations (rdi) */
#define SYS_COM2_READ 0
#define SYS_COM2_WRITE 1

/* SYS_CONS operations (rdi) */
#define SYS_CONS_READ 0
#define SYS_CONS_WRITE 1

/* SYS_SYSINFO operations (rdi) */
#define SYSINFO_PS 0
#define SYSINFO_MEM 1

/* Bytes SYS_CONS will move per call at most (bounds the kernel bounce buffer). */
#define CONS_MAX_BYTES 256

/* Bytes SYS_SYSINFO will produce at most (bounds the kernel format buffer). */
#define SYSINFO_MAX_BYTES 1024

/* Bytes SYS_QRAND will draw per call at most (a capless caller is denied
 * before any draw). */
#define QRAND_MAX_BYTES 64

/* Bytes SYS_COM2 will move per call at most (bounds the kernel bounce buffer). */
#define COM2_MAX_BYTES 256

/* Error returns (in rax) */
#define SYSCALL_EINVAL ((uint64_t) - 1)
#define SYSCALL_EFAULT ((uint64_t) - 2)
#define SYSCALL_ENOSYS ((uint64_t) - 3)
#define SYSCALL_EPERM ((uint64_t) - 4)  /* no capability authorising the op */
#define SYSCALL_EIO ((uint64_t) - 5)    /* delivery failed (e.g. queue full) */
#define SYSCALL_ENOENT ((uint64_t) - 6) /* no such file on the initrd */

/* User memory window: identity-mapped, user-bit pages above the
 * kernel heap. Each user process gets one 2 MB region (code at the
 * base, stack at the top). */
#define USER_REGION_BASE 0x06000000UL /* 96 MB */
#define USER_REGION_SIZE 0x200000UL   /* one 2 MB page */
#define USER_MAX_PROCESSES 4

/* Install the int 0x80 gate and dispatch handler */
void syscall_init(void);

/* Take the latest published field snapshot for rendering. Copies up to
 * FIELD_SNAP_BYTES into `dst`, writes the count to `*out_n`, and returns true
 * iff a new snapshot has arrived since the last take (clears the dirty flag).
 * Consumed by the framebuffer live-field renderer in the idle loop. */
bool field_snapshot_take(int8_t *dst, int *out_n);

/* Map the user regions (sets the user bit on the covering page-table
 * entries), copy the embedded user programs, and spawn them */
void user_init(void);

/* Spawn a user process running a copy of the blob [start, end) in the
 * next free user region. Returns STATUS_* codes. */
status_t user_process_spawn(const char *name, const void *blob_start, const void *blob_end,
                            uint32_t *pid_out);

/* Spawn a user process from an embedded ELF64 image [start, end). */
status_t user_process_spawn_elf(const char *name, const void *elf_start, const void *elf_end,
                                uint32_t *pid_out);

#endif /* SYSCALL_H */
