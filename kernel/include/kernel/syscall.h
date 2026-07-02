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

/* Error returns (in rax) */
#define SYSCALL_EINVAL   ((uint64_t)-1)
#define SYSCALL_EFAULT   ((uint64_t)-2)
#define SYSCALL_ENOSYS   ((uint64_t)-3)

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

#endif /* SYSCALL_H */
