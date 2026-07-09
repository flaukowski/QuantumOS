/**
 * QuantumOS Capability Authority Ledger (epic #133 — Phase D)
 *
 * A kernel-authoritative, append-only record of capability GRANTs, DENIALs, and
 * SPAWNs. The KERNEL writes it — a ring-3 citizen cannot forge or suppress its
 * own entry — so an external verifier can see what authority each agent holds
 * and every attempt to exceed it. This is the "conscience before wallet"
 * substrate: reliability = faithfulness to granted intent, made observable.
 *
 * HONEST SCOPE: this is an AUTHORITY LEDGER, NOT a full syscall/action trace.
 * It records GRANT (a capability minted for a pid), DENY (a capability-gated
 * syscall refused because the caller lacked the right), and SPAWN (a process
 * created). It does NOT record successful exercise of already-held authority,
 * and (v1) does NOT record REVOKE/EXPIRE — so it is an append-only ATTEMPT/GRANT
 * trace, NOT a snapshot of current live holdings. DENY coverage is the set of
 * cap-gated syscalls hooked in syscall.c (quantum, com2, console, field, spawn);
 * IPC and net ownership denials are not yet logged (documented gap).
 *
 * CONCURRENCY: audit_record is called from BOTH syscall context AND the IF=1
 * service health monitor (a watchdog-reborn citizen re-mints caps from that
 * thread — service.c). So unlike field.c it does NOT assume a cli'd caller: the
 * ring append + monotonic seq bump are self-contained under a local
 * irqsave/irqrestore. Single CPU, so that suffices.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_AUDIT_H
#define KERNEL_AUDIT_H

#include <kernel/types.h>

#define AUDIT_LOG_ENTRIES 128 /* ring capacity (evictable, oldest-first) */
#define AUDIT_LINE_MAX 96     /* max formatted bytes per entry line */
#define AUDIT_MAX_BYTES (AUDIT_LOG_ENTRIES * AUDIT_LINE_MAX) /* one-shot text buffer */

/* Record kinds — 1-based so a zeroed .bss slot (kind 0) is never a real entry. */
#define AUDIT_KIND_EMPTY 0
#define AUDIT_GRANT 1   /* a capability was minted for pid (cap_create/cap_derive) */
#define AUDIT_DENY 2    /* a cap-gated syscall was refused (caller lacked the right) */
#define AUDIT_SPAWN 3   /* a process was created (EXECUTE authority exercised) */
#define AUDIT_MDENY 4   /* a HELD capability exceeded declared intent (manifest deny) */
#define AUDIT_QUOTA 5   /* a manifest quota refused the operation (e.g. spawn_max) */
#define AUDIT_CPUKILL 6 /* a process was TERMINATED for exceeding its cpu_limit (epic #144) */

/* Verdicts — 1-based likewise. */
#define AUDIT_V_OK 1
#define AUDIT_V_EPERM 2

/* A cap_find (any-match) denial names no specific resource. */
#define AUDIT_RESOURCE_ANY 0xFFFFFFFFu

/* SYS_AUDIT operations (rdi). */
#define AUDIT_OP_READ 0  /* format all live entries (oldest->newest) into the buf */
#define AUDIT_OP_STATS 1 /* one line: AUDIT: total=<seq> dropped=<n> capacity=<cap> */

typedef struct {
    uint64_t seq;           /* monotonic; strictly increases across the log's life */
    uint64_t tick;          /* boot-relative timer tick of the record */
    uint32_t pid;           /* the acting/owning process */
    uint16_t kind;          /* AUDIT_GRANT/DENY/SPAWN */
    uint16_t verdict;       /* AUDIT_V_OK / AUDIT_V_EPERM */
    uint32_t resource_type; /* cap_resource_type_t; SPAWN uses PROCESS */
    uint32_t resource_id;   /* OPAQUE handle — NEVER an address; ANY = 0xFFFFFFFF */
    uint32_t permissions;   /* CAP_READ/WRITE/EXECUTE/... bits attempted/granted */
    uint32_t count;         /* coalesced repeats: N identical consecutive records */
} audit_entry_t;            /* 40 bytes */

/* Record one authority event. Self-contained (irqsave); safe from any context.
 * Coalesces a record identical to the most-recent one into its `count` so a
 * denial flood cannot evict older GRANT/DENY evidence in O(flood). */
void audit_record(uint16_t kind, uint32_t pid, uint16_t verdict, uint32_t resource_type,
                  uint32_t resource_id, uint32_t permissions);

/* Convenience wrappers used at the hook sites. */
void audit_grant(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_deny(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_spawn(uint32_t parent_pid, uint32_t child_pid);

/* Format all live entries oldest->newest as "AUDIT: ..." text lines into buf
 * (bounded, NUL-terminated). Returns bytes written (excluding the NUL). */
size_t audit_format(char *buf, size_t max);

/* One "AUDIT: total=<seq> dropped=<n> capacity=<cap>" line. dropped is guarded
 * (0 while total <= capacity; total-capacity once the ring has wrapped). */
size_t audit_format_stats(char *buf, size_t max);

#endif /* KERNEL_AUDIT_H */
