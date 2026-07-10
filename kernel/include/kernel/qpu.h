/**
 * QuantumOS QPU job broker (epic #148, quantum-stack Phase 1 A2+A3)
 *
 * The kernel is a BROKER, never a parser: circuit payloads are OPAQUE bytes.
 * The kernel enforces WHO may touch the QPU (capability + intent manifest),
 * HOW MUCH (the manifest qsub quota + a per-owner in-flight cap), and records
 * every authority event in the durable ledger — the physics happens in ring 3
 * (qpud, the executor service, runs the exact integer engine; later backends
 * are dispatched by the SAME executor to the host gateway, Phase 2).
 *
 * AUTHORITY MODEL — two disjoint rights over CAP_RESOURCE_DEVICE/DEVICE_ID_QPU:
 *   CAP_WRITE            = may SUBMIT circuits and POLL own jobs (submitters)
 *   CAP_EXECUTE|CAP_READ = may FETCH pending jobs and COMPLETE them (executor)
 * WRITE and EXECUTE must NEVER co-occur on a QPU cap: a submitter that could
 * COMPLETE could forge its own results; an executor that could SUBMIT could
 * charge quota it does not hold. The grant flags mint exactly one side each
 * (service.c), and both citizens prove the cross-perm denial at boot.
 *
 * LIFECYCLE (single CPU, every op cli'd in syscall context — no locking):
 *   SUBMIT: authorize(WRITE) -> qsub quota precheck -> in-flight cap -> free
 *           slot -> copy circuit -> charge -> AUDIT_QSUBMIT -> job_id.
 *   FETCH:  authorize(EXECUTE) -> oldest PENDING -> RUNNING, stamps the
 *           executor's (pid, generation) for forgery defense + reclamation.
 *   COMPLETE: authorize(EXECUTE) + caller must MATCH the executor stamp.
 *   POLL:   authorize(WRITE) + caller must MATCH the owner stamp; DONE
 *           results are copied out and the slot freed (scrubbed).
 * Death is handled like every other per-process kernel resource — a
 * process_destroy sweep (qpu_on_process_destroy): a dead owner's PENDING/DONE
 * slots are scrubbed+freed, its RUNNING slot is marked ORPHANED (the executor
 * may be mid-execution; its COMPLETE then scrubs+frees); a dead EXECUTOR's
 * RUNNING slots fail closed to DONE/EXECFAIL so the owner's POLL frees them.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_QPU_H
#define KERNEL_QPU_H

#include <kernel/types.h>

/* The QPU device id under CAP_RESOURCE_DEVICE ("QP" in ASCII; device ids
 * need only be unique within the DEVICE resource type — the others are port
 * bases: 0x3F8 console, 0x2F8 com2, 0x1F0 disk, 0x8139 net). */
#define DEVICE_ID_QPU 0x5150

#define QPU_MAX_JOBS 4      /* broker slots (global pool) */
#define QPU_INFLIGHT_MAX 2  /* max live jobs per owner pid (starvation guard) */
#define QPU_CIRCUIT_MAX 256 /* opaque circuit payload bytes */
#define QPU_RESULT_MAX 128  /* opaque result payload bytes */

/* SYS_QPU operations (rdi). */
#define QPU_OP_SUBMIT 0   /* rsi=qpu_submit_req_t*            -> job_id (>=1) */
#define QPU_OP_FETCH 1    /* rsi=qpu_fetch_out_t*             -> job_id or 0  */
#define QPU_OP_COMPLETE 2 /* rsi=qpu_complete_req_t*          -> 0            */
#define QPU_OP_POLL 3     /* rsi=job_id, rdx=qpu_poll_out_t*  -> poll state   */

/* POLL return values (rax). Errors are the negative SYSCALL_* codes. */
#define QPU_POLL_PENDING 1 /* queued, not yet fetched */
#define QPU_POLL_RUNNING 2 /* fetched by the executor */
#define QPU_POLL_DONE 3    /* result copied out; slot freed */

/* Result status inside qpu_poll_out_t/qpu_complete_req_t. */
#define QPU_STATUS_OK 0
#define QPU_STATUS_EXECFAIL 1 /* executor died mid-run (fail closed) or rejected */

/* Job slot states (kernel-internal). */
#define QPU_JOB_FREE 0
#define QPU_JOB_PENDING 1
#define QPU_JOB_RUNNING 2
#define QPU_JOB_DONE 3

/* Ring-crossing structs — twin _Static_asserts in usys.h AND syscall.c
 * (there is no shared header across the ring boundary). */
typedef struct {
    uint32_t circuit_len; /* 1..QPU_CIRCUIT_MAX */
    uint8_t circuit[QPU_CIRCUIT_MAX];
} qpu_submit_req_t; /* 260 bytes */

typedef struct {
    uint32_t job_id;
    uint32_t owner_pid; /* provenance for the executor's log line */
    uint32_t circuit_len;
    uint8_t circuit[QPU_CIRCUIT_MAX];
} qpu_fetch_out_t; /* 268 bytes */

typedef struct {
    uint32_t job_id;
    uint32_t status;     /* QPU_STATUS_* */
    uint32_t result_len; /* 0..QPU_RESULT_MAX */
    uint8_t result[QPU_RESULT_MAX];
} qpu_complete_req_t; /* 140 bytes */

typedef struct {
    uint32_t state;  /* QPU_POLL_* (valid on success) */
    uint32_t status; /* QPU_STATUS_* (valid when state == DONE) */
    uint32_t result_len;
    uint8_t result[QPU_RESULT_MAX];
} qpu_poll_out_t; /* 140 bytes */

/* Broker entry points (kernel/src/qpu.c). Each runs cli'd in syscall
 * context; pointers already validated by the syscall layer. */
uint64_t qpu_submit(uint32_t pid, const qpu_submit_req_t *req);
uint64_t qpu_fetch(uint32_t pid, qpu_fetch_out_t *out);
uint64_t qpu_complete(uint32_t pid, const qpu_complete_req_t *req);
uint64_t qpu_poll(uint32_t pid, uint32_t job_id, qpu_poll_out_t *out);

/* Death sweep — call from process_destroy beside the fd/ipc/cap/manifest
 * sweeps (a dead process can never release its own slots). */
void qpu_on_process_destroy(uint32_t pid);

#endif /* KERNEL_QPU_H */
