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

#define SYSCALL_VECTOR   0x80

/* Syscall numbers */
#define SYS_WRITE   1   /* rdi = NUL-terminated string in user memory */
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_EXIT    4   /* rdi = exit code */
#define SYS_TICKS   5
#define SYS_SEND    6   /* rdi = msg ptr, rsi = len; routed by IPC capability */
#define SYS_RECV    7   /* rdi = buf ptr, rsi = len; returns sender pid (0 = empty) */
#define SYS_HEARTBEAT 8 /* report liveness to the service watchdog */
#define SYS_SVC_RESTARTS 9 /* restart count of the caller's service slot, or -1 */
#define SYS_QRAND   10  /* rdi = buf, rsi = len; fill up to QRAND_MAX_BYTES bytes
                         * from the qseed-mixed quantum generator. Gated on a
                         * CAP_RESOURCE_QUANTUM read capability (EPERM without).
                         * len == 0 is a provenance query: returns 1 if the
                         * kernel booted with a qseed, else 0. */
#define SYS_SEND_TO 11  /* rdi = dest pid, rsi = msg ptr, rdx = len; like
                         * SYS_SEND but to a chosen destination the caller holds
                         * an IPC send-capability for (EPERM if it holds none for
                         * that dest). Lets a service reply to the sender pid
                         * recv handed it rather than a first-match peer. */

/* Bytes SYS_QRAND will draw per call at most (a capless caller is denied
 * before any draw). */
#define QRAND_MAX_BYTES  64

/* Error returns (in rax) */
#define SYSCALL_EINVAL   ((uint64_t)-1)
#define SYSCALL_EFAULT   ((uint64_t)-2)
#define SYSCALL_ENOSYS   ((uint64_t)-3)
#define SYSCALL_EPERM    ((uint64_t)-4)   /* no capability authorising the op */
#define SYSCALL_EIO      ((uint64_t)-5)   /* delivery failed (e.g. queue full) */

/* User memory window: identity-mapped, user-bit pages above the
 * kernel heap. Each user process gets one 2 MB region (code at the
 * base, stack at the top). */
#define USER_REGION_BASE   0x06000000UL   /* 96 MB */
#define USER_REGION_SIZE   0x200000UL     /* one 2 MB page */
#define USER_MAX_PROCESSES 4

/* Install the int 0x80 gate and dispatch handler */
void syscall_init(void);

/* Map the user regions (sets the user bit on the covering page-table
 * entries), copy the embedded user programs, and spawn them */
void user_init(void);

/* Spawn a user process running a copy of the blob [start, end) in the
 * next free user region. Returns STATUS_* codes. */
status_t user_process_spawn(const char *name, const void *blob_start,
                            const void *blob_end, uint32_t *pid_out);

/* Spawn a user process from an embedded ELF64 image [start, end). */
status_t user_process_spawn_elf(const char *name, const void *elf_start,
                                const void *elf_end, uint32_t *pid_out);

#endif /* SYSCALL_H */
