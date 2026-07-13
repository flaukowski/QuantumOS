/**
 * QuantumOS kernel-side syscall ABI twins.
 *
 * The kernel copies of the ring-crossing request/result structs, split out of
 * syscall.c so the ADR-0020 v1 ABI freeze probe (kernel/src/abi_probe_kern.c)
 * can #include and measure them. These are kernel-internal — NOT a shared
 * header across the ring boundary (ADR-0001) — but each must stay byte-identical
 * to its user/usys.h twin, which the _Static_asserts here plus the golden ABI
 * gate both enforce.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef KERNEL_SYSCALL_ABI_H
#define KERNEL_SYSCALL_ABI_H

#include <kernel/types.h>
#include <kernel/qpu.h> /* QPU_CIRCUIT_MAX, QPU_RESULT_MAX */

/* SYS_UDP / SYS_TCP request (user/usys.h udp_req_t): 24 B, buf at offset 16,
 * no padding either side. */
typedef struct __attribute__((packed)) {
    int64_t sock;
    uint8_t ip[4];
    uint16_t port;
    uint16_t len;
    uint64_t buf;
} udp_req_k_t;
_Static_assert(sizeof(udp_req_k_t) == 24, "udp_req_t ABI drift");

/* SYS_CAP_DERIVE request (user/usys.h cap_derive_req_t): 24 B, no padding. */
typedef struct __attribute__((packed)) {
    uint32_t resource_type; /* which of MY OWN caps (by type+id, not a handle) */
    uint32_t resource_id;
    uint32_t permissions; /* the narrowed subset to hand over */
    uint32_t target_pid;  /* the sub-agent (must be an IPC peer) */
    uint64_t expiration;  /* 0 = none/inherit (cap_derive clamps <= parent) */
} cap_derive_req_k_t;
_Static_assert(sizeof(cap_derive_req_k_t) == 24, "cap_derive_req ABI drift");

/* SYS_QPU request/result twins (user/usys.h qpu_*): opaque circuit/result bytes,
 * broker never parses them. Must match user side byte-for-byte. */
typedef struct {
    uint32_t circuit_len;
    uint8_t circuit[QPU_CIRCUIT_MAX];
} qpu_submit_req_k_t;
_Static_assert(sizeof(qpu_submit_req_k_t) == 260, "qpu_submit_req ABI drift");

typedef struct {
    uint32_t job_id;
    uint32_t owner_pid;
    uint32_t circuit_len;
    uint8_t circuit[QPU_CIRCUIT_MAX];
} qpu_fetch_out_k_t;
_Static_assert(sizeof(qpu_fetch_out_k_t) == 268, "qpu_fetch_out ABI drift");

typedef struct {
    uint32_t job_id;
    uint32_t status;
    uint32_t result_len;
    uint8_t result[QPU_RESULT_MAX];
} qpu_complete_req_k_t;
_Static_assert(sizeof(qpu_complete_req_k_t) == 140, "qpu_complete_req ABI drift");

typedef struct {
    uint32_t state;
    uint32_t status;
    uint32_t result_len;
    uint8_t result[QPU_RESULT_MAX];
} qpu_poll_out_k_t;
_Static_assert(sizeof(qpu_poll_out_k_t) == 140, "qpu_poll_out ABI drift");

#endif /* KERNEL_SYSCALL_ABI_H */
